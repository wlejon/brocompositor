// Operations on other applications' windows through Accessibility. Each runs
// on the application's own worker (AppWorker), never on a host thread; an
// application that does not answer within the messaging timeout fails the
// operation after its first call instead of stalling it call by call.
#include "mac/shell_impl.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <thread>

namespace brocompositor::mac {

// Size, then position, then size again: a resize near a display edge is
// clamped by the window server until the window has moved.
AXError set_frame(AXUIElementRef w, const Rect& frame) {
    AXError e = ax::set_size(w, frame.size());
    if (ax::unresponsive(e)) return e;
    e = ax::set_position(w, Point{frame.x, frame.y});
    if (e != kAXErrorSuccess) return e;
    return ax::set_size(w, frame.size());
}

int64_t visible_area(const Rect& r, const std::vector<Rect>& displays) {
    int64_t a = 0;
    for (const Rect& d : displays) a += d.intersected(r).area();
    return a;
}

void await_window_server(uint32_t cgid, const std::function<bool(const sys::CgWindow&)>& done) {
    // The window server's list trails an AX change by a few frames; an
    // operation completes once its result is what query() reports (or after
    // 300 ms: an application may animate, or refuse).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    for (;;) {
        auto w = sys::describe_window(cgid);
        if (!w || done(*w) || std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

namespace {

const CFStringRef kFullScreen = CFSTR("AXFullScreen");

// Where to try parking `frame`, best first: hanging off the bottom-right or
// bottom-left corner of a display with only a corner point on it, the
// window's own display first and corners whose window would cover another
// display last. What macOS 26 actually does with such a request (measured):
// a position that leaves any part of the window on a display is kept
// horizontally (a 1 pt column is fine), one entirely beyond the displays is
// pulled back to leave 40 pt; vertically the title bar is kept within the
// display's visible frame (below the menu bar, above a bottom Dock), so the
// window ends up showing a 1 pt column of title bar plus whatever is below
// it down to the display's bottom edge (1 x 91 pt on a 1496 x 967 display
// with a bottom Dock). The caller reads back where the window went.
std::vector<Point> park_candidates(const Rect& frame, const std::vector<Rect>& displays) {
    std::vector<Rect> order = displays;
    std::stable_partition(order.begin(), order.end(), [&](const Rect& d) { return d.contains(frame.center()); });
    std::vector<Point> clear, covering;
    for (const Rect& d : order) {
        for (Point p : {Point{d.right() - 1, d.bottom() - 1}, Point{d.x - frame.width + 1, d.bottom() - 1}}) {
            Rect r{p.x, p.y, frame.width, frame.height};
            bool covers = false;
            for (const Rect& other : displays)
                if (other != d && other.intersects(r)) covers = true;
            (covers ? covering : clear).push_back(p);
        }
    }
    clear.insert(clear.end(), covering.begin(), covering.end());
    return clear;
}

}  // namespace

std::vector<Rect> ShellBackend::Impl::display_frames() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Rect> out;
    for (const auto& s : screens) out.push_back(s.frame);
    return out;
}

bool ShellBackend::Impl::do_place(AppWorker::Context& c, uint32_t cgid, const Rect& frame) {
    AXUIElementRef w = c.find(cgid);
    if (!w) return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!by_cgid.count(cgid)) return false;
    }
    AXError e = kAXErrorSuccess;
    auto minimized = ax::get_bool(w, kAXMinimizedAttribute, &e);
    c.worker->note(e);
    if (ax::unresponsive(e)) return false;  // a hidden window stays hidden (and journaled)
    if (minimized.value_or(false)) ax::set_bool(w, kAXMinimizedAttribute, false);
    if (ax::get_bool(w, kFullScreen, nullptr).value_or(false)) ax::set_bool(w, kFullScreen, false);
    e = set_frame(w, frame);
    c.worker->note(e);
    if (e != kAXErrorSuccess) return false;
    // An explicit placement also un-hides; the journal forgets the window
    // only now that it is back.
    bool was_hidden = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_cgid.find(cgid);
        if (it != by_cgid.end()) {
            was_hidden = it->second.hidden != Hidden::None;
            it->second.hidden = Hidden::None;
        }
    }
    if (was_hidden) journal_sync();
    if (auto now = ax::frame(w, nullptr))
        await_window_server(cgid, [&](const sys::CgWindow& cg) { return cg.onscreen && cg.frame == *now; });
    wake();
    return true;
}

bool ShellBackend::Impl::do_set_visible(AppWorker::Context& c, uint32_t cgid, bool visible) {
    AXUIElementRef w = c.find(cgid);
    if (!w) return false;
    if (!visible) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_cgid.find(cgid);
            if (it == by_cgid.end()) return false;
            if (it->second.hidden != Hidden::None) return true;
        }
        AXError e = kAXErrorSuccess;
        auto info = ax::info(w, &e);
        c.worker->note(e);
        if (!info) return false;
        if (info->minimized) return true;    // already off screen, by the user's choice
        if (info->fullscreen) return false;  // lives in its own Space; neither parks nor minimizes
        std::vector<Rect> displays = display_frames();
        Hidden method = config.hide_method == HideMethod::Park ? Hidden::Park : Hidden::Minimize;
        std::vector<Point> candidates = park_candidates(info->frame, displays);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_cgid.find(cgid);
            if (it == by_cgid.end()) return false;
            it->second.hidden = method;
            it->second.restore_frame = info->frame;
            Point p = candidates.empty() ? Point{info->frame.x, info->frame.y} : candidates.front();
            it->second.parked_frame = Rect{p.x, p.y, info->frame.width, info->frame.height};
        }
        // Journal first: a kill between here and the move is recoverable.
        journal_sync();
        if (method == Hidden::Park) {
            std::optional<Rect> parked;
            for (Point target : candidates) {
                e = ax::set_position(w, target);
                c.worker->note(e);
                if (ax::unresponsive(e)) return false;  // stays journaled; restore_all / recovery put it back
                auto now = ax::frame(w, nullptr);
                sys::trace("park cgid %u: target %d,%d err %d -> %d,%d %dx%d visible %lld", cgid, target.x, target.y,
                           int(e), now ? now->x : -1, now ? now->y : -1, now ? now->width : -1,
                           now ? now->height : -1, now ? (long long)visible_area(*now, displays) : -1LL);
                if (e == kAXErrorSuccess && now && visible_area(*now, displays) <= kParkedVisibleArea) {
                    parked = now;
                    break;
                }
            }
            if (parked) {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = by_cgid.find(cgid);
                if (it != by_cgid.end()) it->second.parked_frame = *parked;
            } else {
                // The application (or the window server) kept it on screen
                // wherever it went: put it back and minimize instead.
                ax::set_position(w, Point{info->frame.x, info->frame.y});
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto it = by_cgid.find(cgid);
                    if (it != by_cgid.end()) it->second.hidden = Hidden::Minimize;
                }
                journal_sync();
                e = ax::set_bool(w, kAXMinimizedAttribute, true);
                method = Hidden::Minimize;
            }
        } else {
            e = ax::set_bool(w, kAXMinimizedAttribute, true);
        }
        c.worker->note(e);
        journal_sync();
        if (e == kAXErrorSuccess) {
            if (method == Hidden::Park)
                if (auto now = ax::frame(w, nullptr))
                    await_window_server(cgid, [&](const sys::CgWindow& cg) { return cg.frame == *now; });
            if (method == Hidden::Minimize)
                await_window_server(cgid, [](const sys::CgWindow& cg) { return !cg.onscreen; });
        }
        wake();
        return e == kAXErrorSuccess;
    }

    Hidden was = Hidden::None;
    Rect restore;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_cgid.find(cgid);
        if (it == by_cgid.end()) return false;
        was = it->second.hidden;
        restore = it->second.restore_frame;
    }
    AXError e = kAXErrorSuccess;
    switch (was) {
        case Hidden::None: return true;
        case Hidden::Park: e = set_frame(w, restore); break;
        case Hidden::Minimize: e = ax::set_bool(w, kAXMinimizedAttribute, false); break;
    }
    c.worker->note(e);
    if (e != kAXErrorSuccess) return false;  // still hidden, still journaled
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_cgid.find(cgid);
        if (it != by_cgid.end()) it->second.hidden = Hidden::None;
    }
    // Only now (the window is back) does the journal forget it.
    journal_sync();
    if (was == Hidden::Park) {
        if (auto now = ax::frame(w, nullptr))
            await_window_server(cgid, [&](const sys::CgWindow& cg) { return cg.frame == *now; });
    } else {
        await_window_server(cgid, [](const sys::CgWindow& cg) { return cg.onscreen; });
    }
    wake();
    return true;
}

// macOS has no focus-stealing prevention for Accessibility clients: making
// the application frontmost and the window main, then raising it, is what
// every window manager does. Confirmed when the application is frontmost
// (Process Manager) and the window is its key window (AXFocusedWindow); the
// window server's z-order is not the test (a window ordered front without
// activation sits above the key window). A newer request (generation)
// abandons this one between steps. cgid 0: no managed window (the
// application is the neutral target, Finder).
FocusResult ShellBackend::Impl::do_focus(AppWorker::Context& c, uint32_t cgid, uint64_t generation) {
    auto superseded = [&] { return focus_generation.load() != generation; };
    if (superseded()) return FocusResult::Superseded;
    AXUIElementRef w = nullptr;
    if (cgid) {
        w = c.find(cgid);
        if (!w) return FocusResult::NoSuchWindow;
    }
    // Focused: the application is frontmost and the window its key window
    // (what tracking reports).
    auto focused_now = [&] {
        return sys::frontmost_pid() == c.pid && (!cgid || ax::focused_window(c.app.get(), nullptr) == cgid);
    };
    if (focused_now()) return FocusResult::AlreadyFocused;
    // The lock screen owns input: nothing can take focus.
    if (sys::screen_locked()) return FocusResult::Denied;
    AXError e = kAXErrorSuccess;
    if (w && ax::get_bool(w, kAXMinimizedAttribute, &e).value_or(false))
        ax::set_bool(w, kAXMinimizedAttribute, false);
    c.worker->note(e);
    if (ax::unresponsive(e)) return FocusResult::Denied;
    if (superseded()) return FocusResult::Superseded;
    e = ax::set_bool(c.app.get(), kAXFrontmostAttribute, true);
    c.worker->note(e);
    if (ax::unresponsive(e)) return FocusResult::Denied;
    if (w) {
        ax::set_bool(w, kAXMainAttribute, true);
        ax::raise(w);
    }
    for (int i = 0; i < 50; ++i) {
        if (focused_now()) {
            sys::trace("focus pid %u cgid %u confirmed after %d ms", c.pid, cgid, i * 10);
            wake();
            return FocusResult::Focused;
        }
        if (superseded()) return FocusResult::Superseded;
        // AXFrontmost can report success and change nothing (Finder showing
        // only the desktop): after 100 ms ask through NSRunningApplication.
        if (i == 10 && sys::frontmost_pid() != c.pid) {
            sys::activate(c.pid);
            if (w) ax::raise(w);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sys::trace("focus pid %u cgid %u not confirmed: frontmost %u, its key window %u (AXFrontmost err %d)", c.pid,
               cgid, sys::frontmost_pid(), ax::focused_window(c.app.get(), nullptr), int(e));
    return superseded() ? FocusResult::Superseded : FocusResult::Denied;
}

bool ShellBackend::Impl::do_close(AppWorker::Context& c, uint32_t cgid) {
    AXUIElementRef w = c.find(cgid);
    if (!w) return false;
    AXError e = ax::press_close_button(w, c.timeout);
    c.worker->note(e);
    return e == kAXErrorSuccess;
}

}  // namespace brocompositor::mac
