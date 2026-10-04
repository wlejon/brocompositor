#pragma once

#include "brocompositor/types.h"
#include <string>

namespace brocompositor {

struct WindowInfo {
    WindowId id = InvalidWindowId;
    std::string title;
    std::string app_id;
    std::string class_name;
    uint32_t process_id = 0;
    Rect geometry;
    WindowState state = WindowState::None;
    int32_t z_order = 0;
    MonitorId monitor_id = PrimaryMonitorId;
    WorkspaceId workspace_id = 1;
    bool has_capture_texture = false;
    uint64_t capture_handle = 0;

    bool is_minimized() const { return has_state(state, WindowState::Minimized); }
    bool is_maximized() const { return has_state(state, WindowState::Maximized); }
    bool is_fullscreen() const { return has_state(state, WindowState::Fullscreen); }
    bool is_tiled() const { return has_state(state, WindowState::Tiled); }
    bool is_floating() const { return has_state(state, WindowState::Floating); }
    bool is_focused() const { return has_state(state, WindowState::Focused); }
    bool is_hidden() const { return has_state(state, WindowState::Hidden); }
    bool is_pinned() const { return has_state(state, WindowState::Pinned); }
    bool is_visible() const { return !is_minimized() && !is_hidden(); }
};

} // namespace brocompositor
