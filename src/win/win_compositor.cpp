#include "win_compositor.h"
#include <algorithm>

namespace brocompositor {

WinCompositor::WinCompositor()
    : virtual_desktops_(workspace_mgr_) {}

WinCompositor::~WinCompositor() {
    shutdown();
}

bool WinCompositor::initialize(const CompositorContext& ctx) {
    if (is_running_.load()) return true;

    context_ = ctx;

    // Initialize capture pipeline if requested
    if (context_.enable_wgc_capture) {
        capture_.initialize();
    }

    // Set ignore PID on hook manager to ignore our own process
#if defined(_WIN32)
    hook_mgr_.set_ignore_process_id(GetCurrentProcessId());
#endif

    // Start background event hook manager if enabled
    if (context_.enable_window_hooks) {
        hook_mgr_.start([this](const WinHookEvent& ev) {
            this->on_hook_event(ev);
        });

        // Discover existing manageable windows on desktop
        std::vector<WindowId> existing = hook_mgr_.discover_existing_windows();
        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(PrimaryMonitorId);

        for (WindowId id : existing) {
            update_window_cache(id);
            workspace_mgr_.add_window(id, active_ws);
        }

        if (context_.auto_tile_new_windows && !existing.empty()) {
            relayout_workspace(active_ws);
        }
    }

    is_running_.store(true);
    return true;
}

void WinCompositor::shutdown() {
    if (!is_running_.load()) return;

    hook_mgr_.stop();
    capture_.shutdown();

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

WorkspaceId WinCompositor::create_workspace(const std::string& name, MonitorId monitor) {
    return workspace_mgr_.create_workspace(name, monitor);
}

bool WinCompositor::remove_workspace(WorkspaceId id) {
    return workspace_mgr_.remove_workspace(id);
}

bool WinCompositor::switch_workspace(WorkspaceId id) {
    const WorkspaceInfo* target = workspace_mgr_.get_workspace(id);
    if (!target) return false;

    MonitorId mon = target->monitor_id;
    WorkspaceId old_id = workspace_mgr_.get_active_workspace(mon);

    if (!workspace_mgr_.switch_workspace(id)) {
        return false;
    }

    virtual_desktops_.apply_workspace_switch(old_id, id);
    relayout_workspace(id);

    if (workspace_cb_) {
        workspace_cb_(CompositorEvent::WorkspaceSwitched, id);
    }
    return true;
}

WorkspaceId WinCompositor::get_active_workspace(MonitorId monitor) const {
    return workspace_mgr_.get_active_workspace(monitor);
}

const WorkspaceInfo* WinCompositor::get_workspace(WorkspaceId id) const {
    return workspace_mgr_.get_workspace(id);
}

std::vector<WorkspaceId> WinCompositor::get_workspaces(MonitorId monitor) const {
    return workspace_mgr_.get_workspaces_on_monitor(monitor);
}

bool WinCompositor::set_workspace_layout_mode(WorkspaceId id, LayoutMode mode) {
    bool ok = workspace_mgr_.set_workspace_layout_mode(id, mode);
    if (ok) {
        relayout_workspace(id);
        if (workspace_cb_) {
            workspace_cb_(CompositorEvent::LayoutChanged, id);
        }
    }
    return ok;
}

void WinCompositor::relayout_workspace(WorkspaceId workspace_id) {
    const WorkspaceInfo* ws = workspace_mgr_.get_workspace(workspace_id);
    if (!ws) return;

    if (ws->layout_mode == LayoutMode::Floating) {
        return; // Floating layout does not auto-tile
    }

    WorkArea wa = appbar_mgr_.get_work_area(ws->monitor_id);
    std::vector<WindowId> all_windows = workspace_mgr_.get_windows(workspace_id);

    // Filter windows that can be tiled (skip minimized/hidden/floating)
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
        WinWindowOps::set_window_rect(pair.first, pair.second);

        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(pair.first);
        if (it != window_cache_.end()) {
            it->second.geometry = pair.second;
            it->second.state |= WindowState::Tiled;
        }
    }
}

std::vector<WindowId> WinCompositor::get_windows(WorkspaceId workspace_id) const {
    return workspace_mgr_.get_windows(workspace_id);
}

const WindowInfo* WinCompositor::get_window_info(WindowId id) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = window_cache_.find(id);
    return it != window_cache_.end() ? &it->second : nullptr;
}

bool WinCompositor::move_window_to_workspace(WindowId win, WorkspaceId workspace_id) {
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

bool WinCompositor::focus_window(WindowId win) {
    WorkspaceId ws = workspace_mgr_.get_window_workspace(win);
    if (ws != InvalidWorkspaceId) {
        workspace_mgr_.set_active_window(ws, win);
    }
    return WinWindowOps::focus_window(win);
}

bool WinCompositor::close_window(WindowId win) {
    return WinWindowOps::close_window(win);
}

bool WinCompositor::set_window_rect(WindowId win, const Rect& rect) {
    bool ok = WinWindowOps::set_window_rect(win, rect);
    if (ok) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            it->second.geometry = rect;
        }
    }
    return ok;
}

bool WinCompositor::set_window_state(WindowId win, WindowState state) {
    bool ok = WinWindowOps::set_window_state(win, state);
    if (ok) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            it->second.state = state;
        }
    }
    return ok;
}

bool WinCompositor::register_appbar(const AppBarInfo& bar) {
    bool ok = appbar_mgr_.register_appbar(bar);
    if (ok) {
        // Relayout all active workspaces on this monitor
        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(bar.monitor_id);
        if (active_ws != InvalidWorkspaceId) {
            relayout_workspace(active_ws);
        }
        if (workspace_cb_) {
            workspace_cb_(CompositorEvent::AppBarChanged, active_ws);
        }
    }
    return ok;
}

bool WinCompositor::unregister_appbar(AppBarId bar_id) {
    bool ok = appbar_mgr_.unregister_appbar(bar_id);
    if (ok) {
        WorkspaceId active_ws = workspace_mgr_.get_active_workspace(PrimaryMonitorId);
        if (active_ws != InvalidWorkspaceId) {
            relayout_workspace(active_ws);
        }
        if (workspace_cb_) {
            workspace_cb_(CompositorEvent::AppBarChanged, active_ws);
        }
    }
    return ok;
}

std::vector<AppBarInfo> WinCompositor::get_appbars(MonitorId monitor_id) const {
    return appbar_mgr_.get_appbars(monitor_id);
}

WorkArea WinCompositor::get_work_area(MonitorId monitor_id) const {
    return appbar_mgr_.get_work_area(monitor_id);
}

void WinCompositor::set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) {
    appbar_mgr_.set_monitor_bounds(monitor_id, bounds);
    WorkspaceId active_ws = workspace_mgr_.get_active_workspace(monitor_id);
    if (active_ws != InvalidWorkspaceId) {
        relayout_workspace(active_ws);
    }
}

bool WinCompositor::enable_capture(WindowId win) {
    bool ok = capture_.start_capture(win);
    if (ok) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = window_cache_.find(win);
        if (it != window_cache_.end()) {
            it->second.has_capture_texture = true;
            it->second.capture_handle = capture_.get_shared_texture_handle(win);
        }
    }
    return ok;
}

void WinCompositor::disable_capture(WindowId win) {
    capture_.stop_capture(win);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = window_cache_.find(win);
    if (it != window_cache_.end()) {
        it->second.has_capture_texture = false;
        it->second.capture_handle = 0;
    }
}

uint64_t WinCompositor::get_shared_texture_handle(WindowId win) {
    return capture_.get_shared_texture_handle(win);
}

void WinCompositor::set_window_event_callback(WindowEventCallback cb) {
    window_cb_ = std::move(cb);
}

void WinCompositor::set_workspace_event_callback(WorkspaceEventCallback cb) {
    workspace_cb_ = std::move(cb);
}

void WinCompositor::on_hook_event(const WinHookEvent& ev) {
    std::lock_guard<std::mutex> lock(event_queue_mutex_);
    queued_events_.push_back(ev);
}

void WinCompositor::update_window_cache(WindowId id) {
    WindowInfo info;
    if (WinWindowOps::get_window_info(id, info)) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        window_cache_[id] = info;
    }
}

void WinCompositor::poll_events() {
    std::vector<WinHookEvent> events_to_process;
    {
        std::lock_guard<std::mutex> lock(event_queue_mutex_);
        events_to_process.swap(queued_events_);
    }

    for (const auto& ev : events_to_process) {
        switch (ev.type) {
            case WinHookEventType::Created: {
                update_window_cache(ev.window_id);
                WorkspaceId active_ws = workspace_mgr_.get_active_workspace(PrimaryMonitorId);
                workspace_mgr_.add_window(ev.window_id, active_ws);
                if (context_.auto_tile_new_windows) {
                    relayout_workspace(active_ws);
                }
                if (window_cb_) {
                    const WindowInfo* win = get_window_info(ev.window_id);
                    window_cb_(CompositorEvent::WindowCreated, ev.window_id, win);
                }
                break;
            }
            case WinHookEventType::Destroyed: {
                WorkspaceId ws = workspace_mgr_.get_window_workspace(ev.window_id);
                workspace_mgr_.remove_window(ev.window_id);
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    window_cache_.erase(ev.window_id);
                }
                if (ws != InvalidWorkspaceId) {
                    relayout_workspace(ws);
                }
                if (window_cb_) {
                    window_cb_(CompositorEvent::WindowDestroyed, ev.window_id, nullptr);
                }
                break;
            }
            case WinHookEventType::Foreground: {
                update_window_cache(ev.window_id);
                WorkspaceId ws = workspace_mgr_.get_window_workspace(ev.window_id);
                if (ws != InvalidWorkspaceId) {
                    workspace_mgr_.set_active_window(ws, ev.window_id);
                }
                if (window_cb_) {
                    const WindowInfo* win = get_window_info(ev.window_id);
                    window_cb_(CompositorEvent::WindowFocused, ev.window_id, win);
                }
                break;
            }
            case WinHookEventType::MoveSizeEnd: {
                update_window_cache(ev.window_id);
                if (window_cb_) {
                    const WindowInfo* win = get_window_info(ev.window_id);
                    window_cb_(CompositorEvent::WindowMovedResized, ev.window_id, win);
                }
                break;
            }
            case WinHookEventType::MinimizeStart:
            case WinHookEventType::MinimizeEnd: {
                update_window_cache(ev.window_id);
                WorkspaceId ws = workspace_mgr_.get_window_workspace(ev.window_id);
                if (ws != InvalidWorkspaceId) {
                    relayout_workspace(ws);
                }
                if (window_cb_) {
                    const WindowInfo* win = get_window_info(ev.window_id);
                    window_cb_(CompositorEvent::WindowStateChanged, ev.window_id, win);
                }
                break;
            }
        }
    }
}

std::unique_ptr<ICompositor> create_compositor() {
    return std::make_unique<WinCompositor>();
}

} // namespace brocompositor
