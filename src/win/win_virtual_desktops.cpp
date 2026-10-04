#include "win_virtual_desktops.h"
#include "win_window_ops.h"
#include "workspace_manager.h"

namespace brocompositor {

WinVirtualDesktops::WinVirtualDesktops(WorkspaceManager& ws_mgr)
    : ws_mgr_(ws_mgr) {}

bool WinVirtualDesktops::apply_workspace_switch(WorkspaceId from_ws, WorkspaceId to_ws) {
    if (from_ws == to_ws) {
        return true;
    }

    // Hide windows from previous workspace (if not pinned)
    if (from_ws != InvalidWorkspaceId) {
        auto old_windows = ws_mgr_.get_windows(from_ws);
        for (WindowId w : old_windows) {
            if (!is_pinned(w)) {
                hide_window(w);
            }
        }
    }

    // Show windows for new workspace
    if (to_ws != InvalidWorkspaceId) {
        auto new_windows = ws_mgr_.get_windows(to_ws);
        for (WindowId w : new_windows) {
            show_window(w);
        }

        // Focus the active window of the target workspace
        WindowId active = ws_mgr_.get_active_window(to_ws);
        if (active != InvalidWindowId) {
            WinWindowOps::focus_window(active);
        }
    }

    return true;
}

void WinVirtualDesktops::pin_window(WindowId win) {
    if (win != InvalidWindowId) {
        pinned_windows_.insert(win);
        show_window(win);
    }
}

void WinVirtualDesktops::unpin_window(WindowId win) {
    pinned_windows_.erase(win);
}

bool WinVirtualDesktops::is_pinned(WindowId win) const {
    return pinned_windows_.find(win) != pinned_windows_.end();
}

void WinVirtualDesktops::hide_window(WindowId win) {
    // Cloaking is preferred because it keeps window in taskbar/Alt+Tab structure gracefully
    if (!WinWindowOps::set_cloaked(win, true)) {
        WinWindowOps::set_window_state(win, WindowState::Hidden);
    }
}

void WinVirtualDesktops::show_window(WindowId win) {
    if (!WinWindowOps::set_cloaked(win, false)) {
        WinWindowOps::set_window_state(win, WindowState::None);
    }
}

} // namespace brocompositor
