// Focus within Windows' foreground rules. The test process starts in the
// background (the terminal owns the foreground), the windows belong to yet
// another background process, so plain SetForegroundWindow is refused and the
// backend's unlock strategies are what get exercised. The user's foreground
// window is restored at the end.
//
// A user working on the machine at the same time legitimately defeats focus
// stealing (their input re-arms Windows' foreground lock, their click moves
// the foreground). Low-level hooks count real, non-injected input: an
// attempt that fails while the user produced input is retried after the user
// goes quiet; an attempt that fails with no user input is a failure as
// before. If the user keeps interfering, the test skips (77) and says so.
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>

using namespace brocompositor;
using namespace bctest;

namespace {

uint64_t native(HWND h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

const char* name(win::FocusResult r) {
    switch (r) {
        case win::FocusResult::Focused: return "Focused";
        case win::FocusResult::AlreadyFocused: return "AlreadyFocused";
        case win::FocusResult::Denied: return "Denied";
        case win::FocusResult::NoSuchWindow: return "NoSuchWindow";
        case win::FocusResult::Superseded: return "Superseded";
        case win::FocusResult::Unavailable: return "Unavailable";
    }
    return "?";
}

struct Ctx {
    win::ShellBackend& shell;
    EventLog& log;
    UserInputMonitor& user;
    int disturbed = 0;   // attempts spoiled by the user
    bool gave_up = false;
};

// Waits until the user has produced no input for `quiet` (at most `limit`).
void wait_user_quiet(Ctx& c, std::chrono::milliseconds quiet = 400ms, std::chrono::milliseconds limit = 8000ms) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    uint64_t last = c.user.count();
    auto since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        Sleep(50);
        uint64_t now = c.user.count();
        if (now != last) {
            last = now;
            since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - since >= quiet) {
            return;
        }
    }
}

// One focus step: `expect_ok` results, the window really foreground, and the
// FocusChanged fact. Retries only attempts the user disturbed.
void focus_step(Ctx& c, const char* label, WindowId id, HWND h,
                std::initializer_list<win::FocusResult> expect_ok, bool expect_event = true) {
    if (c.gave_up) return;
    for (int attempt = 0; attempt < 5; ++attempt) {
        uint64_t before = c.user.count();
        c.log.mark();
        auto r = c.shell.focus(id).get();
        bool result_ok = false;
        for (auto e : expect_ok) result_ok |= r == e;
        bool fg_ok = h ? GetForegroundWindow() == h : true;
        bool event_ok = !expect_event ||
                        c.log.wait<FocusChanged>([&](const FocusChanged& e) { return e.id == id; }).has_value();
        bool user_moved = c.user.count() != before;
        std::printf("  %s -> %s%s\n", label, name(r), user_moved ? " (user input during the attempt)" : "");
        if (result_ok && fg_ok && event_ok) return;
        if (!user_moved) {
            CHECK(result_ok);
            CHECK(fg_ok);
            CHECK(event_ok);
            return;
        }
        ++c.disturbed;
        wait_user_quiet(c);
    }
    c.gave_up = true;
}

int run() {
    ForegroundGuard guard;
    UserInputMonitor user;
    if (!user.active()) std::printf("  (no low-level hooks: every failure counts)\n");
    TestApp app;
    if (!app.start()) return 1;
    Rect wa = primary_work_area();
    HWND a = app.create("a", Rect{wa.x + 80, wa.y + 80, 500, 350}, "aa3333");
    HWND b = app.create("b", Rect{wa.x + 620, wa.y + 80, 500, 350}, "33aa33");
    if (!a || !b) return 1;

    std::string err;
    auto shell = win::ShellBackend::create(test_shell_config(app.pid()), &err);
    if (!shell) return 1;
    EventLog log(shell->events());
    auto wa_ = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == native(a); });
    auto wb_ = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == native(b); });
    if (!wa_ || !wb_) return 1;
    WindowId ia = wa_->window.id, ib = wb_->window.id;
    Ctx c{*shell, log, user};
    using FR = win::FocusResult;

    for (int round = 0; round < 3; ++round) {
        focus_step(c, ("round " + std::to_string(round) + " a").c_str(), ia, a, {FR::Focused, FR::AlreadyFocused});
        focus_step(c, ("round " + std::to_string(round) + " b").c_str(), ib, b, {FR::Focused, FR::AlreadyFocused});
    }
    focus_step(c, "a", ia, a, {FR::Focused});
    focus_step(c, "a again", ia, a, {FR::AlreadyFocused}, false);

    // Focusing a minimized window restores it.
    if (!c.gave_up) {
        CHECK(app.ok("minimize b"));
        CHECK(wait_until([&] { return IsIconic(b) != FALSE; }));
        focus_step(c, "minimized b", ib, b, {FR::Focused});
        CHECK(!IsIconic(b));
    }

    // A later request supersedes one that has not finished: the last one
    // asked for wins.
    if (!c.gave_up) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            uint64_t before = user.count();
            auto first = shell->focus(ia);
            auto second = shell->focus(ib);
            auto r1 = first.get(), r2 = second.get();
            std::printf("  a then b at once -> %s, %s\n", name(r1), name(r2));
            bool ok = (r1 == FR::Focused || r1 == FR::Superseded) &&
                      (r2 == FR::Focused || r2 == FR::AlreadyFocused) && GetForegroundWindow() == b;
            if (ok) break;
            if (user.count() == before) {
                CHECK(ok);
                break;
            }
            ++c.disturbed;
            wait_user_quiet(c);
            if (attempt == 4) c.gave_up = true;
        }
    }

    // "Focus nothing": the foreground leaves every managed window and the
    // backend reports focus on no window.
    if (!c.gave_up) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            uint64_t before = user.count();
            log.mark();
            auto none = shell->focus(kNoWindow).get();
            std::printf("  focus none -> %s\n", name(none));
            HWND fg = GetForegroundWindow();
            bool ok = none == FR::Focused && fg != a && fg != b &&
                      log.wait<FocusChanged>([](const FocusChanged& e) { return e.id == kNoWindow; }).has_value();
            if (ok) break;
            if (user.count() == before) {
                CHECK(none == FR::Focused);
                CHECK(fg != a && fg != b);
                break;
            }
            ++c.disturbed;
            wait_user_quiet(c);
            if (attempt == 4) c.gave_up = true;
        }
    }

    CHECK(shell->focus(987654).get() == FR::NoSuchWindow);

    if (c.gave_up) {
        std::printf("[test_win_focus] SKIPPED: the user kept using the desktop (%d disturbed attempts); "
                    "focus could not be judged\n", c.disturbed);
        return 77;
    }
    if (c.disturbed) std::printf("  %d attempt(s) retried after user input\n", c.disturbed);
    return 0;
}

}  // namespace

int main() {
    bctest::require_mutate("test_win_focus", "takes the foreground from the user");
    init_windows_test();
    if (!interactive_desktop()) {
        std::printf("[test_win_focus] SKIPPED: the input desktop is not the user's Default desktop "
                    "(screen saver / lock screen); nothing can take the foreground\n");
        return 77;
    }
    int rc = run();
    if (rc == 77 && bctest::failures() == 0) return 77;
    if (rc != 0 && bctest::failures() == 0) return rc;
    return finish("test_win_focus");
}
