// The desktop-shell role: the shell hook's HSHELL_FLASH arrives as
// WindowAttention for a tracked window; a window attached as the shell
// window is kept beneath the applications' windows until it is raised, and
// goes back beneath them when lowered. Under Explorer (every interactive run)
// SetShellWindow is refused, so the session's shell is never touched.
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace brocompositor;
using namespace bctest;

namespace {

uint64_t native(HWND h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

// Whether `upper` is above `lower` in the z-order.
bool above(HWND upper, HWND lower) {
    for (HWND h = GetWindow(upper, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT))
        if (h == lower) return true;
    return false;
}

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

HWND own_window(const Rect& r) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"bc_test_shell_window";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
        RegisterClassW(&wc);
        registered = true;
    }
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"bc_test_shell_window", L"shell", WS_POPUP, r.x, r.y, r.width,
                             r.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h) ShowWindow(h, SW_SHOWNOACTIVATE);
    pump();
    return h;
}

void attention() {
    TestApp app;
    REQUIRE(app.start());
    Rect wa = primary_work_area();
    auto shell = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
    REQUIRE(shell);
    EventLog log(shell->events());
    HWND w = app.create("flashy", Rect{wa.x + 80, wa.y + 80, 400, 300}, "aa6633");
    REQUIRE(w);
    auto added = log.wait<WindowAdded>([w](const WindowAdded& e) { return e.window.native == native(w); });
    REQUIRE(added.has_value());

    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = w;
    fi.dwFlags = FLASHW_TRAY;
    fi.uCount = 1;
    FlashWindowEx(&fi);
    auto flash = log.wait<WindowAttention>([&](const WindowAttention& e) { return e.id == added->window.id; });
    CHECK(flash.has_value());
}

void shell_window() {
    ForegroundGuard guard;
    TestApp app;
    REQUIRE(app.start());
    Rect wa = primary_work_area();
    HWND app_window = app.create("app", Rect{wa.x + 100, wa.y + 100, 400, 300}, "3366aa");
    REQUIRE(app_window);

    auto shell = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
    REQUIRE(shell);
    CHECK(!shell->set_shell_raised(true));  // nothing attached

    // Only a window of this process, on its own thread.
    CHECK(!shell->attach_desktop_shell(native(app_window)));
    CHECK(!shell->attach_desktop_shell(0));

    HWND own = own_window(Rect{wa.x + 140, wa.y + 140, 300, 200});
    REQUIRE(own);
    REQUIRE(shell->attach_desktop_shell(native(own)));
    CHECK(!shell->attach_desktop_shell(native(own)));  // already attached
    // Explorer holds the shell window in any interactive session.
    CHECK_EQ(shell->is_desktop_shell(), GetShellWindow() == own);

    // Pinned: raising it the ordinary way leaves it beneath the application.
    SetWindowPos(own, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pump();
    CHECK(above(app_window, own));

    // Raised for an overlay: above it; lowered: beneath it again. Raising
    // takes the foreground from the user.
    const char* mutate = std::getenv("BROCOMPOSITOR_TEST_MUTATE");
    if (mutate && std::string(mutate) == "1") {
        CHECK(shell->set_shell_raised(true));
        pump();
        CHECK(above(own, app_window));
        CHECK(shell->set_shell_raised(false));
        pump();
        CHECK(above(app_window, own));
    } else {
        std::printf("  raise / lower: skipped (takes the foreground; BROCOMPOSITOR_TEST_MUTATE=1 runs it)\n");
    }

    // Gone with its window: another one can be attached.
    DestroyWindow(own);
    pump();
    HWND again = own_window(Rect{wa.x + 160, wa.y + 160, 200, 150});
    REQUIRE(again);
    CHECK(shell->attach_desktop_shell(native(again)));
    DestroyWindow(again);
    pump();
}

}  // namespace

int main() {
    init_windows_test();
    if (!interactive_desktop()) {
        std::printf("[test_win_shell] SKIPPED: the input desktop is not the user's\n");
        return 77;
    }
    attention();
    shell_window();
    return finish("test_win_shell");
}
