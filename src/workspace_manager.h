#pragma once

#include "brocompositor/workspace.h"
#include "brocompositor/window.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

namespace brocompositor {

class WorkspaceManager {
public:
    WorkspaceManager();
    ~WorkspaceManager() = default;

    WorkspaceId create_workspace(const std::string& name, MonitorId monitor_id = PrimaryMonitorId);
    bool remove_workspace(WorkspaceId id);

    bool switch_workspace(WorkspaceId id);
    WorkspaceId get_active_workspace(MonitorId monitor_id = PrimaryMonitorId) const;

    const WorkspaceInfo* get_workspace(WorkspaceId id) const;
    WorkspaceInfo* get_workspace_mut(WorkspaceId id);
    std::vector<WorkspaceId> get_workspaces_on_monitor(MonitorId monitor_id) const;

    bool set_workspace_layout_mode(WorkspaceId id, LayoutMode mode);

    // Window assignment
    bool add_window(WindowId win, WorkspaceId workspace_id);
    bool remove_window(WindowId win);
    bool move_window_to_workspace(WindowId win, WorkspaceId target_workspace);
    WorkspaceId get_window_workspace(WindowId win) const;

    // Focus & active window
    bool set_active_window(WorkspaceId workspace_id, WindowId win);
    WindowId get_active_window(WorkspaceId workspace_id) const;

    // Monitor assignment
    void set_workspace_monitor(WorkspaceId workspace_id, MonitorId monitor_id);

    // List all windows in workspace
    std::vector<WindowId> get_windows(WorkspaceId workspace_id) const;

private:
    mutable std::mutex mutex_;
    WorkspaceId next_workspace_id_ = 1;

    std::unordered_map<WorkspaceId, WorkspaceInfo> workspaces_;
    std::unordered_map<MonitorId, WorkspaceId> active_workspace_by_monitor_;
    std::unordered_map<WindowId, WorkspaceId> window_workspace_map_;
};

} // namespace brocompositor
