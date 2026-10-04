#pragma once

#include "brocompositor/types.h"
#include "brocompositor/workspace.h"
#include <unordered_set>
#include <vector>

namespace brocompositor {

class WorkspaceManager;

class WinVirtualDesktops {
public:
    explicit WinVirtualDesktops(WorkspaceManager& ws_mgr);
    ~WinVirtualDesktops() = default;

    // Perform the OS-level visibility transition when switching workspaces
    bool apply_workspace_switch(WorkspaceId from_ws, WorkspaceId to_ws);

    // Pin a window so it remains visible across all workspaces
    void pin_window(WindowId win);
    void unpin_window(WindowId win);
    bool is_pinned(WindowId win) const;

    // Show / Hide helper for window belonging to a workspace
    void hide_window(WindowId win);
    void show_window(WindowId win);

private:
    WorkspaceManager& ws_mgr_;
    std::unordered_set<WindowId> pinned_windows_;
};

} // namespace brocompositor
