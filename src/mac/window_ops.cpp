// Operations on other applications' windows through Accessibility. Each runs
// on the application's own worker (AppWorker), never on a host thread; an
// application that does not answer within the messaging timeout fails the
// operation after its first call instead of stalling it call by call.
#include "mac/shell_impl.h"

#include <algorithm>
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

namespace {

const CFStringRef kFullScreen = CFSTR("AXFullScreen");

// Where to park `frame`: hanging off the bottom-right (or bottom-left)
// corner of a display, with only its top-left (top-right) point on it, and
// covering no other display. The window's own display first.
Point park_point(const Rect& frame, const std::vector<Rect>& displays) {
    std::vector<Rect> order = displays;
    std::stable_partition(order.begin(), order.end(), [&](const Rect& d) { return d.contains(frame.center()); });
    std::optional<Point> fallback;
    for (const Rect& d : order) {
        for (Point p : {Point{d.right() - 1, d.bottom() - 1}, Point{d.x - frame.width + 1, d.bottom() - 1}}) {
            Rect r{p.x, p.y, frame.width, frame.height};
            if (!fallback) fallback = p;
            bool clear = true;
            for (const Rect& other : displays)
                if (other != d && other.intersects(r)) clear = false;
            if (clear) return p;
        }
    }
    return fallback.value_or(Point{frame.x, frame.y});
}

// The front-most normal window of `pid` in the window server's order.
uint32_t front_window_of(uint32_t pid) {
    for (const auto& w : sys::window_list(true))
        if (w.layer == 0 && w.pid == pid) return w.id;
    return 0;
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
    Hidden was = Hidden::None;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_cgid.find(cgid);
        if (it == by_cgid.end()) return false;
        was = it->second.hidden;
        it->second.hidden = Hidden::None;  // an explicit placement also un-hides
    }
    if (was != Hidden::None) journal_sync();
    AXError e = kAXErrorSuccess;
    auto minimized = ax::get_bool(w, kAXMinimizedAttribute, &e);
    c.worker->note(e);
    if (ax::unresponsive(e)) return false;
    if (minimized.value_or(false)) ax::set_bool(w, kAXMinimizedAttribute, false);
    if (ax::get_bool(w, kFullScreen, nullptr).value_or(false)) ax::set_bool(w, kFullScreen, false);
    e = set_frame(w, frame);
    c.worker->note(e);
    wake();
    return e == kAXErrorSuccess;
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
        Point target = park_point(info->frame, displays);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_cgid.find(cgid);
            if (it == by_cgid.end()) return false;
            it->second.hidden = method;
            it->second.restore_frame = info->frame;
            it->second.parked_frame = Rect{target.x, target.y, info->frame.width, info->frame.height};
        }
        // Journal first: a kill between here and the move is recoverable.
        journal_sync();
        if (method == Hidden::Park) {
            e = ax::set_position(w, target);
            c.worker->note(e);
            if (ax::unresponsive(e)) return false;  // stays journaled; restore_all / recovery put it back
            auto now = ax::frame(w, nullptr);
            if (e == kAXErrorSuccess && now && visible_area(*now, displays) <= kParkedVisibleArea) {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = by_cgid.find(cgid);
                if (it != by_cgid.end()) it->second.parked_frame = *now;
            } else {
                // The application (or the window server) kept it on screen:
                // put it back and minimize instead.
                ax::set_position(w, Point{info->frame.x, info->frame.y});
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto it = by_cgid.find(cgid);
                    if (it != by_cgid.end()) it->second.hidden = Hidden::Minimize;
                }
                journal_sync();
                e = ax::set_bool(w, kAXMinimizedAttribute, true);
            }
        } else {
            e = ax::set_bool(w, kAXMinimizedAttribute, true);
        }
        c.worker->note(e);
        journal_sync();
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
    wake();
    return true;
}

// macOS has no focus-stealing prevention for Accessibility clients: making
// the application frontmost and the window main, then raising it, is what
// every window manager does. The window server confirms it (the application
// is frontmost and the window first in its order). A newer request
// (generation) abandons this one between steps. cgid 0: no managed window
// (the application is the neutral target, Finder).
FocusResult ShellBackend::Impl::do_focus(AppWorker::Context& c, uint32_t cgid, uint64_t generation) {
    auto superseded = [&] { return focus_generation.load() != generation; };
    if (superseded()) return FocusResult::Superseded;
    AXUIElementRef w = nullptr;
    if (cgid) {
        w = c.find(cgid);
        if (!w) return FocusResult::NoSuchWindow;
    }
    auto focused_now = [&] {
        return sys::frontmost_pid() == c.pid && (!cgid || front_window_of(c.pid) == cgid);
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
            wake();
            return FocusResult::Focused;
        }
        if (superseded()) return FocusResult::Superseded;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return superseded() ? FocusResult::Superseded : FocusResult::Denied;
}

bool ShellBackend::Impl::do_close(AppWorker::Context& c, uint32_t cgid) {
    AXUIElementRef w = c.find(cgid);
    if (!w) return false;
    AXError e = ax::press_close_button(w);
    c.worker->note(e);
    return e == kAXErrorSuccess;
}

}  // namespace brocompositor::mac
