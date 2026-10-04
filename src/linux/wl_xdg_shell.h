#pragma once

#include "wl_types.h"
#include <unordered_map>
#include <mutex>
#include <functional>

namespace brocompositor {

class WlBackend;

struct XdgToplevelSurface {
    WindowId id = InvalidWindowId;
    std::string title;
    std::string app_id;
    Rect geometry;
    Rect requested_geometry;
    WindowState state = WindowState::None;
    bool mapped = false;
    uint32_t configure_serial = 0;
    wlr_xdg_toplevel* wlr_toplevel = nullptr;
    wlr_xdg_surface* wlr_surface = nullptr;
};

struct XdgPopupSurface {
    WindowId id = InvalidWindowId;
    WindowId parent_id = InvalidWindowId;
    Rect geometry;
    bool mapped = false;
    wlr_xdg_popup* wlr_popup = nullptr;
    wlr_xdg_surface* wlr_surface = nullptr;
};

class WlXdgShell {
public:
    using WindowCreatedCallback = std::function<void(WindowId id, const WindowInfo& info)>;
    using WindowDestroyedCallback = std::function<void(WindowId id)>;
    using WindowStateCallback = std::function<void(WindowId id, WindowState state)>;
    using WindowGeometryCallback = std::function<void(WindowId id, const Rect& rect)>;
    using WindowCommitCallback = std::function<void(WindowId id)>;

    WlXdgShell();
    ~WlXdgShell();

    WlXdgShell(const WlXdgShell&) = delete;
    WlXdgShell& operator=(const WlXdgShell&) = delete;

    bool initialize(WlBackend& backend);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Surface registration & management (used by Wayland listeners or mock tests)
    WindowId create_toplevel(const std::string& app_id, const std::string& title, const Rect& initial_rect);
    WindowId create_popup(WindowId parent_id, const Rect& relative_rect);
    bool map_surface(WindowId id);
    bool unmap_surface(WindowId id);
    bool destroy_surface(WindowId id);
    bool commit_surface(WindowId id);

    // Compositor-to-client configuration
    bool configure_toplevel(WindowId id, const Rect& geometry, WindowState state);
    bool close_toplevel(WindowId id);
    bool set_toplevel_state(WindowId id, WindowState state);

    // Querying
    const XdgToplevelSurface* get_toplevel(WindowId id) const;
    const XdgPopupSurface* get_popup(WindowId id) const;
    bool get_window_info(WindowId id, WindowInfo& out_info) const;
    std::vector<WindowId> get_all_toplevel_ids() const;
    std::vector<WindowId> get_popups_for_parent(WindowId parent_id) const;

    // Client requests handling
    void on_client_request_maximize(WindowId id, bool maximize);
    void on_client_request_fullscreen(WindowId id, bool fullscreen);
    void on_client_request_minimize(WindowId id);
    void on_client_request_move(WindowId id);
    void on_client_request_resize(WindowId id, Edge edge);

    // Callbacks
    void set_window_created_callback(WindowCreatedCallback cb);
    void set_window_destroyed_callback(WindowDestroyedCallback cb);
    void set_window_state_callback(WindowStateCallback cb);
    void set_window_geometry_callback(WindowGeometryCallback cb);
    void set_window_commit_callback(WindowCommitCallback cb);

private:
    bool initialized_ = false;
    wlr_xdg_shell* xdg_shell_ = nullptr;

    mutable std::mutex mutex_;
    WindowId next_window_id_ = 1000;

    std::unordered_map<WindowId, XdgToplevelSurface> toplevels_;
    std::unordered_map<WindowId, XdgPopupSurface> popups_;

    WindowCreatedCallback created_cb_;
    WindowDestroyedCallback destroyed_cb_;
    WindowStateCallback state_cb_;
    WindowGeometryCallback geometry_cb_;
    WindowCommitCallback commit_cb_;
};

} // namespace brocompositor
