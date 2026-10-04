#pragma once

#include "brocompositor/export.h"
#include "brocompositor/types.h"
#include "brocompositor/window.h"
#include "brocompositor/workspace.h"
#include "brocompositor/appbar.h"
#include "brocompositor/layout.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace brocompositor {

enum class CompositorEvent {
    WindowCreated,
    WindowDestroyed,
    WindowFocused,
    WindowMovedResized,
    WindowStateChanged,
    WorkspaceSwitched,
    LayoutChanged,
    AppBarChanged,
    CaptureFrameAvailable
};

using WindowEventCallback = std::function<void(CompositorEvent, WindowId, const WindowInfo*)>;
using WorkspaceEventCallback = std::function<void(CompositorEvent, WorkspaceId)>;

struct CompositorContext {
    bool enable_window_hooks = true;
    bool enable_wgc_capture = false;
    bool auto_tile_new_windows = true;
    LayoutConfig default_layout_config;
    std::vector<MonitorId> monitor_ids = {PrimaryMonitorId};
};

class BROCOMPOSITOR_API ICompositor {
public:
    virtual ~ICompositor() = default;

    // Lifecycle
    virtual bool initialize(const CompositorContext& ctx) = 0;
    virtual void shutdown() = 0;
    virtual bool is_running() const = 0;

    // Workspace management
    virtual WorkspaceId create_workspace(const std::string& name, MonitorId monitor = PrimaryMonitorId) = 0;
    virtual bool remove_workspace(WorkspaceId id) = 0;
    virtual bool switch_workspace(WorkspaceId id) = 0;
    virtual WorkspaceId get_active_workspace(MonitorId monitor = PrimaryMonitorId) const = 0;
    virtual const WorkspaceInfo* get_workspace(WorkspaceId id) const = 0;
    virtual std::vector<WorkspaceId> get_workspaces(MonitorId monitor = PrimaryMonitorId) const = 0;
    virtual bool set_workspace_layout_mode(WorkspaceId id, LayoutMode mode) = 0;
    virtual void relayout_workspace(WorkspaceId workspace_id) = 0;

    // Window management
    virtual std::vector<WindowId> get_windows(WorkspaceId workspace_id) const = 0;
    virtual const WindowInfo* get_window_info(WindowId id) const = 0;
    virtual bool move_window_to_workspace(WindowId win, WorkspaceId workspace_id) = 0;
    virtual bool focus_window(WindowId win) = 0;
    virtual bool close_window(WindowId win) = 0;
    virtual bool set_window_rect(WindowId win, const Rect& rect) = 0;
    virtual bool set_window_state(WindowId win, WindowState state) = 0;

    // Edge AppBars
    virtual bool register_appbar(const AppBarInfo& bar) = 0;
    virtual bool unregister_appbar(AppBarId bar_id) = 0;
    virtual std::vector<AppBarInfo> get_appbars(MonitorId monitor_id = PrimaryMonitorId) const = 0;
    virtual WorkArea get_work_area(MonitorId monitor_id = PrimaryMonitorId) const = 0;
    virtual void set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) = 0;

    // Capture / Vulkan zero-copy ingestion
    virtual bool enable_capture(WindowId win) = 0;
    virtual void disable_capture(WindowId win) = 0;
    virtual uint64_t get_shared_texture_handle(WindowId win) = 0;

    // Event handling
    virtual void set_window_event_callback(WindowEventCallback cb) = 0;
    virtual void set_workspace_event_callback(WorkspaceEventCallback cb) = 0;
    virtual void poll_events() = 0;
};

BROCOMPOSITOR_API std::unique_ptr<ICompositor> create_compositor();

} // namespace brocompositor
