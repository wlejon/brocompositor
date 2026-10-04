#pragma once

#include "brocompositor/compositor.h"
#include "workspace_manager.h"
#include "layout_engine.h"
#include "appbar_manager.h"
#include "win_hook_manager.h"
#include "win_window_ops.h"
#include "win_virtual_desktops.h"
#include "win_capture.h"

#include <queue>
#include <mutex>
#include <atomic>

namespace brocompositor {

class WinCompositor : public ICompositor {
public:
    WinCompositor();
    ~WinCompositor() override;

    // Lifecycle
    bool initialize(const CompositorContext& ctx) override;
    void shutdown() override;
    bool is_running() const override { return is_running_.load(); }

    // Workspace management
    WorkspaceId create_workspace(const std::string& name, MonitorId monitor = PrimaryMonitorId) override;
    bool remove_workspace(WorkspaceId id) override;
    bool switch_workspace(WorkspaceId id) override;
    WorkspaceId get_active_workspace(MonitorId monitor = PrimaryMonitorId) const override;
    const WorkspaceInfo* get_workspace(WorkspaceId id) const override;
    std::vector<WorkspaceId> get_workspaces(MonitorId monitor = PrimaryMonitorId) const override;
    bool set_workspace_layout_mode(WorkspaceId id, LayoutMode mode) override;
    void relayout_workspace(WorkspaceId workspace_id) override;

    // Window management
    std::vector<WindowId> get_windows(WorkspaceId workspace_id) const override;
    const WindowInfo* get_window_info(WindowId id) const override;
    bool move_window_to_workspace(WindowId win, WorkspaceId workspace_id) override;
    bool focus_window(WindowId win) override;
    bool close_window(WindowId win) override;
    bool set_window_rect(WindowId win, const Rect& rect) override;
    bool set_window_state(WindowId win, WindowState state) override;

    // Edge AppBars
    bool register_appbar(const AppBarInfo& bar) override;
    bool unregister_appbar(AppBarId bar_id) override;
    std::vector<AppBarInfo> get_appbars(MonitorId monitor_id = PrimaryMonitorId) const override;
    WorkArea get_work_area(MonitorId monitor_id = PrimaryMonitorId) const override;
    void set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) override;

    // Capture
    bool enable_capture(WindowId win) override;
    void disable_capture(WindowId win) override;
    uint64_t get_shared_texture_handle(WindowId win) override;

    // Event handling
    void set_window_event_callback(WindowEventCallback cb) override;
    void set_workspace_event_callback(WorkspaceEventCallback cb) override;
    void poll_events() override;

private:
    void on_hook_event(const WinHookEvent& ev);
    void update_window_cache(WindowId id);

    std::atomic<bool> is_running_{false};
    CompositorContext context_;

    WorkspaceManager workspace_mgr_;
    LayoutEngine layout_engine_;
    AppBarManager appbar_mgr_;
    WinHookManager hook_mgr_;
    WinVirtualDesktops virtual_desktops_;
    WinGraphicsCapture capture_;

    WindowEventCallback window_cb_;
    WorkspaceEventCallback workspace_cb_;

    mutable std::mutex cache_mutex_;
    std::unordered_map<WindowId, WindowInfo> window_cache_;

    mutable std::mutex event_queue_mutex_;
    std::vector<WinHookEvent> queued_events_;
};

} // namespace brocompositor
