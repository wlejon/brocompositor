// Focus within Windows' foreground rules. The test process starts in the
// background (the terminal owns the foreground), the windows belong to yet
// another background process, so plain SetForegroundWindow is refused and the
// backend's unlock strategies are what get exercised. The user's foreground
// window is restored at the end.
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
    }
    return "?";
}

void run() {
    ForegroundGuard guard;
    TestApp app;
    REQUIRE(app.start());
    Rect wa = primary_work_area();
    HWND a = app.create("a", Rect{wa.x + 80, wa.y + 80, 500, 350}, "aa3333");
    HWND b = app.create("b", Rect{wa.x + 620, wa.y + 80, 500, 350}, "33aa33");
    REQUIRE(a && b);

    win::ShellConfig cfg;
    cfg.process_filter = {app.pid()};
    std::string err;
    auto shell = win::ShellBackend::create(cfg, &err);
    REQUIRE(shell);
    EventLog log(shell->events());
    auto wa_ = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == native(a); });
    auto wb_ = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == native(b); });
    REQUIRE(wa_ && wb_);
    WindowId ia = wa_->window.id, ib = wb_->window.id;

    for (int round = 0; round < 3; ++round) {
        for (auto [id, h] : {std::pair{ia, a}, std::pair{ib, b}}) {
            auto r = shell->focus(id);
            std::printf("  round %d focus %s -> %s\n", round, h == a ? "a" : "b", name(r));
            CHECK(r == win::FocusResult::Focused || r == win::FocusResult::AlreadyFocused);
            CHECK_EQ(GetForegroundWindow(), h);
            auto f = log.wait<FocusChanged>([&](const FocusChanged& e) { return e.id == id; });
            CHECK(f.has_value());
        }
    }
    CHECK(shell->focus(ia) == win::FocusResult::Focused);
    CHECK(shell->focus(ia) == win::FocusResult::AlreadyFocused);

    // Focusing a minimized window restores it.
    CHECK(app.ok("minimize b"));
    CHECK(wait_until([&] { return IsIconic(b) != FALSE; }));
    auto r = shell->focus(ib);
    CHECK(r == win::FocusResult::Focused);
    CHECK(!IsIconic(b));
    CHECK_EQ(GetForegroundWindow(), b);

    // "Focus nothing": the foreground leaves every managed window and the
    // backend reports focus on no window.
    log.mark();
    auto none = shell->focus(kNoWindow);
    std::printf("  focus none -> %s\n", name(none));
    CHECK(none == win::FocusResult::Focused);
    CHECK(GetForegroundWindow() != a && GetForegroundWindow() != b);
    CHECK(log.wait<FocusChanged>([](const FocusChanged& e) { return e.id == kNoWindow; }).has_value());

    CHECK(shell->focus(987654) == win::FocusResult::NoSuchWindow);
}

}  // namespace

int main() {
    init_windows_test();
    if (!interactive_desktop()) {
        std::printf("[test_win_focus] SKIPPED: the input desktop is not the user's Default desktop "
                    "(screen saver / lock screen); nothing can take the foreground\n");
        return 77;
    }
    run();
    return finish("test_win_focus");
}
