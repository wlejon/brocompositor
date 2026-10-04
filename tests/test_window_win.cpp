#include "win_window_ops.h"
#include "win_virtual_desktops.h"
#include "win_appbar.h"
#include "workspace_manager.h"
#include "brocompositor/window.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_window_win] Running Win32 window operations tests...\n";

    // 1. Test WindowState flags & operators
    brocompositor::WindowState s = brocompositor::WindowState::None;
    assert(!brocompositor::has_state(s, brocompositor::WindowState::Focused));

    s |= brocompositor::WindowState::Focused;
    assert(brocompositor::has_state(s, brocompositor::WindowState::Focused));

    s |= brocompositor::WindowState::Tiled;
    assert(brocompositor::has_state(s, brocompositor::WindowState::Focused));
    assert(brocompositor::has_state(s, brocompositor::WindowState::Tiled));

    s &= ~brocompositor::WindowState::Focused;
    assert(!brocompositor::has_state(s, brocompositor::WindowState::Focused));
    assert(brocompositor::has_state(s, brocompositor::WindowState::Tiled));

    // 2. Test WindowInfo predicates
    brocompositor::WindowInfo info;
    info.id = 0x12345678;
    info.title = "Test Window";
    info.geometry = {100, 100, 800, 600};
    info.state = brocompositor::WindowState::Maximized | brocompositor::WindowState::Focused;

    assert(info.is_maximized());
    assert(info.is_focused());
    assert(!info.is_minimized());
    assert(!info.is_floating());
    assert(info.is_visible());

    info.state = brocompositor::WindowState::Minimized;
    assert(info.is_minimized());
    assert(!info.is_visible());

    // 3. HWND <-> WindowId conversions
    void* fake_hwnd = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEF));
    brocompositor::WindowId id = brocompositor::WinWindowOps::hwnd_to_id(fake_hwnd);
    assert(brocompositor::WinWindowOps::id_to_hwnd(id) == fake_hwnd);

    // 4. WinWindowOps with invalid HWND shouldn't crash and should return false
    brocompositor::WindowId invalid_id = 0;
    assert(!brocompositor::WinWindowOps::set_window_rect(invalid_id, {0, 0, 100, 100}));
    assert(!brocompositor::WinWindowOps::focus_window(invalid_id));
    assert(!brocompositor::WinWindowOps::close_window(invalid_id));
    assert(!brocompositor::WinWindowOps::is_manageable_window(invalid_id));

    // 5. Test WinVirtualDesktops pinning logic
    brocompositor::WorkspaceManager ws_mgr;
    brocompositor::WinVirtualDesktops vdesktops(ws_mgr);

    brocompositor::WindowId pinned_win = 9999;
    assert(!vdesktops.is_pinned(pinned_win));
    vdesktops.pin_window(pinned_win);
    assert(vdesktops.is_pinned(pinned_win));
    vdesktops.unpin_window(pinned_win);
    assert(!vdesktops.is_pinned(pinned_win));

    // 6. Test WinAppBar system work area query
    brocompositor::Rect sys_wa = brocompositor::WinAppBar::get_system_work_area();
    assert(sys_wa.width > 0 && sys_wa.height > 0);

    std::cout << "[test_window_win] PASSED\n";
    return 0;
}
