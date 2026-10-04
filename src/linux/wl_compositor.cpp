#include "wl_compositor.h"
#include <algorithm>

namespace brocompositor {

LinuxCompositor::LinuxCompositor() = default;

LinuxCompositor::~LinuxCompositor() {
    shutdown();
}

bool LinuxCompositor::initialize(const CompositorContext& ctx) {
    if (is_running_.load()) return true;

    context_ = ctx;

    WlBackend::Config b_cfg;
    b_cfg.mode = WlBackend::BackendMode::Auto;
    b_cfg.socket_name = "wayland-bro-0";
    b_cfg.start_thread = true;

    if (!backend_.initialize(b_cfg)) {
        return false;
    }

    if (!seat_.initialize(backend_)) {
        backend_.shutdown();
        return false;
    }

    if (!xdg_shell_.initialize(backend_)) {
        seat_.shutdown();
        backend_.shutdown();
        return false;
    }

    if (!layer_shell_.initialize(backend_)) {
        xdg_shell_.shutdown();
        seat_.shutdown();
        backend_.shutdown();
        return false;
    }

    if (!dmabuf_vulkan_.initialize(backend_)) {
        layer_shell_.shutdown();
        xdg_shell_.shutdown();
        seat_.shutdown();
        backend_.shutdown();
        return false;
    }

    wire_subsystem_callbacks();

    // Setup initial monitors and default workspace
    for (MonitorId m : context_.monitor_ids) {
        appbar_mgr_.set_monitor_bounds(m, {0, 0, 1920, 1080});
        if (workspace_mgr_.get_workspaces_on_monitor(m).empty()) {
            workspace_mgr_.create_workspace("Default", m);
        }
    }

    is_running_.store(true);
    return true;
}

void LinuxCompositor::shutdown() {
    if (!is_running_.load()) return;

    dmabuf_vulkan_.shutdown();
    layer_shell_.shutdown();
    xdg_shell_.shutdown();
    seat_.shutdown();
    backend_.shutdown();

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        window_cache_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(event_queue_mutex_);
        queued_events_.clear();
    }

    is_running_.store(false);
}

void LinuxCompositor::wire_subsystem_callbacks() {
    // 1. Layer Shell exclusive zone updates -> AppBarManager & relayout
    layer_shell_.set_exclusive_zone_callback([this](AppBarId id, MonitorId monitor, bool active, const AppBarInfo* bar) {
        if (active && bar) {
            appbar_mgr_.register_appbar(*bar);
        } else {
            appbar_mgr_.unregister_appbar(id);
        }

        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(monitor);
        if (active_ws != InvalidWorkspaceId) {
            relayout_workspace(active_ws);
        }
        queue_event(CompositorEvent::AppBarChanged, InvalidWindowId, active_ws);
    });

    // 2. Foreign window creation via XDG shell
    xdg_shell_.set_window_created_callback([this](WindowId id, const WindowInfo& info) {
        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(PrimaryMonitorId);
        workspace_mgr_.add_window(id, active_ws);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            window_cache_[id] = info;
            window_cache_[id].workspace_id = active_ws;
        }

        if (context_.auto_tile_new_windows) {
            relayout_workspace(active_ws);
        }

        queue_event(CompositorEvent::WindowCreated, id, active_ws);
    });

    // 3. Foreign window destruction
    xdg_shell_.set_window_destroyed_callback([this](WindowId id) {
        WorkspaceId ws = workspace_mgr_.get_window_workspace(id);
        workspace_mgr_.remove_window(id);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            window_cache_.erase(id);
        }

        if (ws != InvalidWorkspaceId) {
            relayout_workspace(ws);
        }

        queue_event(CompositorEvent::WindowDestroyed, id, ws);
    });

    // 4. Foreign window state update
    xdg_shell_.set_window_state_callback([this](WindowId id, WindowState state) {
        WorkspaceId ws = InvalidWorkspaceId;
        bool state_changed = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = window_cache_.find(id);
            if (it != window_cache_.end()) {
                if (it->second.state != state) {
                    it->second.state = state;
                    state_changed = true;
                }
                ws = it->second.workspace_id;
            }
        }

        if (state_changed && ws != InvalidWorkspaceId) {
            relayout_workspace(ws);
        }

        queue_event(CompositorEvent::WindowStateChanged, id, ws);
    });

    // 5. Foreign window geometry update
    xdg_shell_.set_window_geometry_callback([this](WindowId id, const Rect& rect) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = window_cache_.find(id);
            if (it != window_cache_.end()) {
                it->second.geometry = rect;
            }
        }
        queue_event(CompositorEvent::WindowMovedResized, id);
    });

    // 6. Keyboard focus in seat
    seat_.set_focus_callback([this](WindowId /*old_focus*/, WindowId new_focus) {
        if (new_focus != InvalidWindowId) {
            WorkspaceId ws = workspace_mgr_.get_window_workspace(new_focus);
            if (ws != InvalidWorkspaceId) {
                workspace_mgr_.set_active_window(ws, new_focus);
            }
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                auto it = window_cache_.find(new_focus);
                if (it != window_cache_.end()) {
                    it->second.state |= WindowState::Focused;
                }
            }
            queue_event(CompositorEvent::WindowFocused, new_focus, ws);
        }
    });

    // 7. Touchpad 3-finger swipe gestures for workspace switching
    seat_.set_swipe_action_callback([this](int fingers, Edge direction) {
        handle_swipe_gesture(fingers, direction);
    });
}

void LinuxCompositor::handle_swipe_gesture(int fingers, Edge direction) {
    if (fingers != 3 && fingers != 4) return;

    // Horizontal swipe switches workspaces
    if (direction != Edge::Left && direction != Edge::Right) return;

    WorkspaceId active_ws = workspace_mgr_.get_active_workspace(PrimaryMonitorId);
    auto all_workspaces = workspace_mgr_.get_workspaces_on_monitor(PrimaryMonitorId);
    if (all_workspaces.size() <= 1) return;

    auto it = std::find(all_workspaces.begin(), all_workspaces.end(), active_ws);
    if (it == all_workspaces.end()) return;

    size_t idx = std::distance(all_workspaces.begin(), it);
    if (direction == Edge::Right) {
        // Swipe Right -> switch to next workspace
        size_t next_idx = (idx + 1) % all_workspaces.size();
        switch_workspace(all_workspaces[next_idx]);
    } else {
        // Swipe Left -> switch to previous workspace
        size_t prev_idx = (idx == 0) ? (all_workspaces.size() - 1) : (idx - 1);
        switch_workspace(all_workspaces[prev_idx]);
    }
}

void LinuxCompositor::queue_event(CompositorEvent event, WindowId win, WorkspaceId ws) {
    std::lock_guard<std::mutex> lock(event_queue_mutex_);
    queued_events_.push_back({event, win, ws});
}

WorkspaceId LinuxCompositor::create_workspace(const std::string& name, MonitorId monitor) {
    return workspace_mgr_.create_workspace(name, monitor);
}

bool LinuxCompositor::remove_workspace(WorkspaceId id) {
    return workspace_mgr_.remove_workspace(id);
}

bool LinuxCompositor::switch_workspace(WorkspaceId id) {
    const WorkspaceInfo* target = workspace_mgr_.get_workspace(id);
    if (!target) return false;

    if (!workspace_mgr_.switch_workspace(id)) {
        return false;
    }

    relayout_workspace(id);
    queue_event(CompositorEvent::WorkspaceSwitched, InvalidWindowId, id);

    if (workspace_cb_) {
        workspace_cb_(CompositorEvent::WorkspaceSwitched, id);
    }
    return true;
}

WorkspaceId LinuxCompositor::get_active_workspace(MonitorId monitor) const {
    return workspace_mgr_.get_active_workspace(monitor);
}

const WorkspaceInfo* LinuxCompositor::get_workspace(WorkspaceId id) const {
    return workspace_mgr_.get_workspace(id);
}

std::vector<WorkspaceId> LinuxCompositor::get_workspaces(MonitorId monitor) const {
    return workspace_mgr_.get_workspaces_on_monitor(monitor);
}

bool LinuxCompositor::set_workspace_layout_mode(WorkspaceId id, LayoutMode mode) {
    bool ok = workspace_mgr_.set_workspace_layout_mode(id, mode);
    if (ok) {
        relayout_workspace(id);
        queue_event(CompositorEvent::LayoutChanged, InvalidWindowId, id);
        if (workspace_cb_) {
            workspace_cb_(CompositorEvent::LayoutChanged, id);
        }
    }
    return ok;
}

void LinuxCompositor::relayout_workspace(WorkspaceId workspace_id) {
    bool expected = false;
    if (!in_relayout_.compare_exchange_strong(expected, true)) {
        return;
    }
    struct RelayoutGuard {
        std::atomic<bool>& flag;
        ~RelayoutGuard() { flag.store(false); }
    } guard{in_relayout_};

    const WorkspaceInfo* ws = workspace_mgr_.get_workspace(workspace_id);
    if (!ws) return;

    if (ws->layout_mode == LayoutMode::Floating) {
        return;
    }

    WorkArea wa = appbar_mgr_.get_work_area(ws->monitor_id);
    std::vector<WindowId> all_windows = workspace_mgr_.get_windows(workspace_id);

    std::vector<WindowId> tile_windows;
    for (WindowId w : all_windows) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(w);
        if (it != window_cache_.end()) {
            if (it->second.is_minimized() || it->second.is_hidden() || it->second.is_floating()) {
                continue;
            }
        }
        tile_windows.push_back(w);
    }

    if (tile_windows.empty()) return;

    LayoutResult result = layout_engine_.compute_layout(
        ws->layout_mode,
        wa.available_work_area,
        tile_windows,
        context_.default_layout_config
    );

    for (const auto& pair : result.placements) {
        WindowId win = pair.first;
        Rect rect = pair.second;

        WindowState state = WindowState::Tiled;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = window_cache_.find(win);
            if (it != window_cache_.end()) {
                it->second.geometry = rect;
                it->second.state |= WindowState::Tiled;
                state = it->second.state;
            }
        }

        xdg_shell_.configure_toplevel(win, rect, state);
    }
}

std::vector<WindowId> LinuxCompositor::get_windows(WorkspaceId workspace_id) const {
    return workspace_mgr_.get_windows(workspace_id);
}

const WindowInfo* LinuxCompositor::get_window_info(WindowId id) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = window_cache_.find(id);
    return it != window_cache_.end() ? &it->second : nullptr;
}

bool LinuxCompositor::move_window_to_workspace(WindowId win, WorkspaceId workspace_id) {
    WorkspaceId old_ws = workspace_mgr_.get_window_workspace(win);
    bool ok = workspace_mgr_.move_window_to_workspace(win, workspace_id);
    if (ok) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = window_cache_.find(win);
            if (it != window_cache_.end()) {
                it->second.workspace_id = workspace_id;
            }
        }
        relayout_workspace(old_ws);
        relayout_workspace(workspace_id);
    }
    return ok;
}

bool LinuxCompositor::focus_window(WindowId win) {
    WorkspaceId ws = workspace_mgr_.get_window_workspace(win);
    if (ws != InvalidWorkspaceId) {
        workspace_mgr_.set_active_window(ws, win);
    }
    return seat_.set_focus(win);
}

bool LinuxCompositor::close_window(WindowId win) {
    xdg_shell_.close_toplevel(win);
    return xdg_shell_.destroy_surface(win);
}

bool LinuxCompositor::set_window_rect(WindowId win, const Rect& rect) {
    WindowState state = WindowState::None;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            it->second.geometry = rect;
            state = it->second.state;
        }
    }
    return xdg_shell_.configure_toplevel(win, rect, state);
}

bool LinuxCompositor::set_window_state(WindowId win, WindowState state) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            it->second.state = state;
        }
    }
    return xdg_shell_.set_toplevel_state(win, state);
}

bool LinuxCompositor::register_appbar(const AppBarInfo& bar) {
    bool ok = appbar_mgr_.register_appbar(bar);
    if (ok) {
        LayerAnchor anchor = LayerAnchor::None;
        switch (bar.edge) {
            case Edge::Top:    anchor = LayerAnchor::Top | LayerAnchor::Left | LayerAnchor::Right; break;
            case Edge::Bottom: anchor = LayerAnchor::Bottom | LayerAnchor::Left | LayerAnchor::Right; break;
            case Edge::Left:   anchor = LayerAnchor::Left | LayerAnchor::Top | LayerAnchor::Bottom; break;
            case Edge::Right:  anchor = LayerAnchor::Right | LayerAnchor::Top | LayerAnchor::Bottom; break;
        }

        layer_shell_.create_layer_surface(
            bar.name,
            bar.monitor_id,
            LayerType::Top,
            anchor,
            {bar.bounds.width, bar.bounds.height},
            bar.thickness
        );

        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(bar.monitor_id);
        if (active_ws != InvalidWorkspaceId) {
            relayout_workspace(active_ws);
        }
        queue_event(CompositorEvent::AppBarChanged, InvalidWindowId, active_ws);
    }
    return ok;
}

bool LinuxCompositor::unregister_appbar(AppBarId bar_id) {
    bool ok = appbar_mgr_.unregister_appbar(bar_id);
    if (ok) {
        layer_shell_.destroy_layer_surface(bar_id);
        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(PrimaryMonitorId);
        if (active_ws != InvalidWorkspaceId) {
            relayout_workspace(active_ws);
        }
        queue_event(CompositorEvent::AppBarChanged, InvalidWindowId, active_ws);
    }
    return ok;
}

std::vector<AppBarInfo> LinuxCompositor::get_appbars(MonitorId monitor_id) const {
    return appbar_mgr_.get_appbars(monitor_id);
}

WorkArea LinuxCompositor::get_work_area(MonitorId monitor_id) const {
    return appbar_mgr_.get_work_area(monitor_id);
}

void LinuxCompositor::set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) {
    appbar_mgr_.set_monitor_bounds(monitor_id, bounds);
    backend_.set_output_bounds(monitor_id, bounds);
    layer_shell_.arrange_layers(monitor_id, bounds);

    WorkspaceId active_ws = workspace_mgr_.get_active_workspace(monitor_id);
    if (active_ws != InvalidWorkspaceId) {
        relayout_workspace(active_ws);
    }
}

bool LinuxCompositor::enable_capture(WindowId win) {
    Rect geom{0, 0, 1920, 1080};
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end() && !it->second.geometry.empty()) {
            geom = it->second.geometry;
        }
    }

    DmaBufAttributes attribs;
    attribs.width = geom.width > 0 ? geom.width : 1920;
    attribs.height = geom.height > 0 ? geom.height : 1080;
    attribs.format = DRM_FOURCC_ARGB8888;
    attribs.modifier = DRM_MODIFIER_LINEAR;
    attribs.n_planes = 1;
    attribs.planes[0].fd = 100; // Simulated plane 0 descriptor
    attribs.planes[0].stride = static_cast<uint32_t>(attribs.width * 4);
    attribs.planes[0].offset = 0;

    auto tex = dmabuf_vulkan_.import_dmabuf(attribs);
    if (!tex) return false;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            it->second.has_capture_texture = true;
            it->second.capture_handle = tex->image_handle;
        }
    }

    queue_event(CompositorEvent::CaptureFrameAvailable, win);
    return true;
}

void LinuxCompositor::disable_capture(WindowId win) {
    uint64_t handle = 0;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            handle = it->second.capture_handle;
            it->second.has_capture_texture = false;
            it->second.capture_handle = 0;
        }
    }

    if (handle != 0) {
        dmabuf_vulkan_.release_texture(handle);
    }
}

uint64_t LinuxCompositor::get_shared_texture_handle(WindowId win) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = window_cache_.find(win);
    return (it != window_cache_.end()) ? it->second.capture_handle : 0;
}

void LinuxCompositor::set_window_event_callback(WindowEventCallback cb) {
    window_cb_ = std::move(cb);
}

void LinuxCompositor::set_workspace_event_callback(WorkspaceEventCallback cb) {
    workspace_cb_ = std::move(cb);
}

void LinuxCompositor::poll_events() {
    backend_.process_pending();

    std::vector<QueuedCompositorEvent> events;
    {
        std::lock_guard<std::mutex> lock(event_queue_mutex_);
        events.swap(queued_events_);
    }

    for (const auto& qe : events) {
        if (qe.event == CompositorEvent::WorkspaceSwitched ||
            qe.event == CompositorEvent::LayoutChanged ||
            qe.event == CompositorEvent::AppBarChanged) {
            if (workspace_cb_) {
                workspace_cb_(qe.event, qe.workspace_id);
            }
        } else {
            if (window_cb_) {
                const WindowInfo* info = get_window_info(qe.window_id);
                window_cb_(qe.event, qe.window_id, info);
            }
        }
    }
}

std::unique_ptr<LinuxCompositor> create_linux_compositor() {
    return std::make_unique<LinuxCompositor>();
}

#if !defined(_WIN32)
std::unique_ptr<ICompositor> create_compositor() {
    return std::make_unique<LinuxCompositor>();
}
#endif

} // namespace brocompositor
