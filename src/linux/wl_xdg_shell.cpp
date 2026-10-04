#include "wl_xdg_shell.h"
#include "wl_backend.h"

namespace brocompositor {

WlXdgShell::WlXdgShell() = default;

WlXdgShell::~WlXdgShell() {
    shutdown();
}

bool WlXdgShell::initialize(WlBackend& backend) {
    if (initialized_) return true;

#if defined(BRO_HAS_WAYLAND)
    if (backend.get_display()) {
        xdg_shell_ = wlr_xdg_shell_create(backend.get_display(), 3);
    }
#else
    (void)backend;
    xdg_shell_ = reinterpret_cast<wlr_xdg_shell*>(static_cast<uintptr_t>(0x4000));
#endif

    initialized_ = true;
    return true;
}

void WlXdgShell::shutdown() {
    if (!initialized_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        popups_.clear();
        toplevels_.clear();
    }

#if defined(BRO_HAS_WAYLAND)
    // wlroots cleans up xdg_shell with display
    xdg_shell_ = nullptr;
#else
    xdg_shell_ = nullptr;
#endif

    initialized_ = false;
}

WindowId WlXdgShell::create_toplevel(const std::string& app_id, const std::string& title, const Rect& initial_rect) {
    std::lock_guard<std::mutex> lock(mutex_);
    WindowId id = next_window_id_++;

    XdgToplevelSurface top;
    top.id = id;
    top.app_id = app_id;
    top.title = title;
    top.geometry = initial_rect;
    top.requested_geometry = initial_rect;
    top.state = WindowState::None;
    top.mapped = false;

    toplevels_[id] = std::move(top);
    return id;
}

WindowId WlXdgShell::create_popup(WindowId parent_id, const Rect& relative_rect) {
    std::lock_guard<std::mutex> lock(mutex_);
    WindowId id = next_window_id_++;

    XdgPopupSurface pop;
    pop.id = id;
    pop.parent_id = parent_id;
    pop.geometry = relative_rect;
    pop.mapped = false;

    popups_[id] = std::move(pop);
    return id;
}

bool WlXdgShell::map_surface(WindowId id) {
    WindowInfo info;
    WindowCreatedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it_top = toplevels_.find(id);
        if (it_top != toplevels_.end()) {
            it_top->second.mapped = true;
            // Populate WindowInfo for creation callback
            info.id = it_top->second.id;
            info.app_id = it_top->second.app_id;
            info.class_name = it_top->second.app_id;
            info.title = it_top->second.title;
            info.geometry = it_top->second.geometry;
            info.state = it_top->second.state;
            cb = created_cb_;
        } else {
            auto it_pop = popups_.find(id);
            if (it_pop != popups_.end()) {
                it_pop->second.mapped = true;
                return true;
            }
            return false;
        }
    }

    if (cb) {
        cb(id, info);
    }
    return true;
}

bool WlXdgShell::unmap_surface(WindowId id) {
    WindowState s = WindowState::None;
    WindowStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it_top = toplevels_.find(id);
        if (it_top != toplevels_.end()) {
            it_top->second.mapped = false;
            it_top->second.state &= ~WindowState::Focused;
            s = it_top->second.state;
            cb = state_cb_;
        } else {
            auto it_pop = popups_.find(id);
            if (it_pop != popups_.end()) {
                it_pop->second.mapped = false;
                return true;
            }
            return false;
        }
    }

    if (cb) {
        cb(id, s);
    }
    return true;
}

bool WlXdgShell::destroy_surface(WindowId id) {
    WindowDestroyedCallback cb;
    std::vector<WindowId> child_popups_to_remove;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it_top = toplevels_.find(id);
        if (it_top != toplevels_.end()) {
            toplevels_.erase(it_top);
            cb = destroyed_cb_;

            // Find child popups
            for (const auto& pair : popups_) {
                if (pair.second.parent_id == id) {
                    child_popups_to_remove.push_back(pair.first);
                }
            }
            for (WindowId pid : child_popups_to_remove) {
                popups_.erase(pid);
            }
        } else {
            auto it_pop = popups_.find(id);
            if (it_pop != popups_.end()) {
                popups_.erase(it_pop);
                return true;
            }
            return false;
        }
    }

    if (cb) {
        cb(id);
    }
    return true;
}

bool WlXdgShell::commit_surface(WindowId id) {
    WindowCommitCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (toplevels_.find(id) != toplevels_.end() || popups_.find(id) != popups_.end()) {
            cb = commit_cb_;
        } else {
            return false;
        }
    }
    if (cb) {
        cb(id);
    }
    return true;
}

bool WlXdgShell::configure_toplevel(WindowId id, const Rect& geometry, WindowState state) {
    WindowGeometryCallback g_cb;
    WindowStateCallback s_cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end()) {
            return false;
        }

        bool geom_changed = (it->second.geometry != geometry);
        bool state_changed = (it->second.state != state);

        it->second.geometry = geometry;
        it->second.state = state;
        it->second.configure_serial++;

#if defined(BRO_HAS_WAYLAND)
        if (it->second.wlr_toplevel) {
            wlr_xdg_toplevel_set_size(it->second.wlr_toplevel, geometry.width, geometry.height);
            wlr_xdg_toplevel_set_maximized(it->second.wlr_toplevel, has_state(state, WindowState::Maximized));
            wlr_xdg_toplevel_set_fullscreen(it->second.wlr_toplevel, has_state(state, WindowState::Fullscreen));
            wlr_xdg_toplevel_set_activated(it->second.wlr_toplevel, has_state(state, WindowState::Focused));
        }
        if (it->second.wlr_surface) {
            wlr_xdg_surface_schedule_configure(it->second.wlr_surface);
        }
#endif

        if (geom_changed) {
            g_cb = geometry_cb_;
        }
        if (state_changed) {
            s_cb = state_cb_;
        }
    }

    if (g_cb) {
        g_cb(id, geometry);
    }
    if (s_cb) {
        s_cb(id, state);
    }
    return true;
}

bool WlXdgShell::close_toplevel(WindowId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = toplevels_.find(id);
    if (it == toplevels_.end()) {
        return false;
    }

#if defined(BRO_HAS_WAYLAND)
    if (it->second.wlr_toplevel) {
        wlr_xdg_toplevel_send_close(it->second.wlr_toplevel);
    }
#endif
    return true;
}

bool WlXdgShell::set_toplevel_state(WindowId id, WindowState state) {
    WindowStateCallback s_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end()) {
            return false;
        }
        it->second.state = state;
        s_cb = state_cb_;
    }
    if (s_cb) {
        s_cb(id, state);
    }
    return true;
}

const XdgToplevelSurface* WlXdgShell::get_toplevel(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = toplevels_.find(id);
    return it != toplevels_.end() ? &it->second : nullptr;
}

const XdgPopupSurface* WlXdgShell::get_popup(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = popups_.find(id);
    return it != popups_.end() ? &it->second : nullptr;
}

bool WlXdgShell::get_window_info(WindowId id, WindowInfo& out_info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = toplevels_.find(id);
    if (it == toplevels_.end()) {
        return false;
    }

    out_info.id = it->second.id;
    out_info.title = it->second.title;
    out_info.app_id = it->second.app_id;
    out_info.class_name = it->second.app_id;
    out_info.geometry = it->second.geometry;
    out_info.state = it->second.state;
    out_info.process_id = 0;
    out_info.monitor_id = PrimaryMonitorId;
    out_info.workspace_id = 1;
    out_info.has_capture_texture = false;
    out_info.capture_handle = 0;
    return true;
}

std::vector<WindowId> WlXdgShell::get_all_toplevel_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WindowId> ids;
    ids.reserve(toplevels_.size());
    for (const auto& pair : toplevels_) {
        ids.push_back(pair.first);
    }
    return ids;
}

std::vector<WindowId> WlXdgShell::get_popups_for_parent(WindowId parent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WindowId> ids;
    for (const auto& pair : popups_) {
        if (pair.second.parent_id == parent_id) {
            ids.push_back(pair.first);
        }
    }
    return ids;
}

void WlXdgShell::on_client_request_maximize(WindowId id, bool maximize) {
    WindowState s = WindowState::None;
    WindowStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toplevels_.find(id);
        if (it != toplevels_.end()) {
            if (maximize) {
                it->second.state |= WindowState::Maximized;
                it->second.state &= ~WindowState::Minimized;
            } else {
                it->second.state &= ~WindowState::Maximized;
            }
            s = it->second.state;
            cb = state_cb_;
        }
    }
    if (cb) {
        cb(id, s);
    }
}

void WlXdgShell::on_client_request_fullscreen(WindowId id, bool fullscreen) {
    WindowState s = WindowState::None;
    WindowStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toplevels_.find(id);
        if (it != toplevels_.end()) {
            if (fullscreen) {
                it->second.state |= WindowState::Fullscreen;
            } else {
                it->second.state &= ~WindowState::Fullscreen;
            }
            s = it->second.state;
            cb = state_cb_;
        }
    }
    if (cb) {
        cb(id, s);
    }
}

void WlXdgShell::on_client_request_minimize(WindowId id) {
    WindowState s = WindowState::None;
    WindowStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toplevels_.find(id);
        if (it != toplevels_.end()) {
            it->second.state |= WindowState::Minimized;
            it->second.state &= ~WindowState::Focused;
            s = it->second.state;
            cb = state_cb_;
        }
    }
    if (cb) {
        cb(id, s);
    }
}

void WlXdgShell::on_client_request_move(WindowId /*id*/) {
    // Interactive move request from client
}

void WlXdgShell::on_client_request_resize(WindowId /*id*/, Edge /*edge*/) {
    // Interactive resize request from client
}

void WlXdgShell::set_window_created_callback(WindowCreatedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    created_cb_ = std::move(cb);
}

void WlXdgShell::set_window_destroyed_callback(WindowDestroyedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    destroyed_cb_ = std::move(cb);
}

void WlXdgShell::set_window_state_callback(WindowStateCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_cb_ = std::move(cb);
}

void WlXdgShell::set_window_geometry_callback(WindowGeometryCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    geometry_cb_ = std::move(cb);
}

void WlXdgShell::set_window_commit_callback(WindowCommitCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    commit_cb_ = std::move(cb);
}

} // namespace brocompositor
