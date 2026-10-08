// The fact vocabulary a platform backend reports and the policy core consumes.
//
// Every event is a self-contained value: a backend snapshots the OS state at
// the moment it noticed the change and pushes the snapshot into an
// EventQueue; the host drains the queue on its own thread. Nothing in an
// event points back into backend storage, so events can be held, copied,
// and replayed (the core's tests are written entirely in these terms).
//
// Ids are minted by the backend and never reused within a process: a native
// window handle that is recycled by the OS gets a fresh WindowId.
#pragma once

#include "brocompositor/geometry.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace brocompositor {

using WindowId = uint64_t;
using MonitorId = uint32_t;
using WorkspaceId = uint32_t;
using ReservationId = uint64_t;

inline constexpr WindowId kNoWindow = 0;
inline constexpr MonitorId kNoMonitor = 0;
inline constexpr WorkspaceId kNoWorkspace = 0;
inline constexpr ReservationId kNoReservation = 0;

struct MonitorSnapshot {
    MonitorId id = kNoMonitor;
    std::string name;        // stable device name (\\.\DISPLAY1, DP-1, ...)
    Rect bounds;             // full output rectangle
    Rect work_area;          // bounds minus reserved edges (taskbar, appbars, panels)
    uint32_t dpi = 96;       // effective DPI; scale = dpi / 96
    bool primary = false;
    uint64_t native = 0;     // platform handle (HMONITOR on Windows); informational only

    bool operator==(const MonitorSnapshot&) const = default;
};

struct WindowSnapshot {
    WindowId id = kNoWindow;
    WindowId owner = kNoWindow;  // transient parent (dialogs) when the owner is tracked
    uint64_t native = 0;         // platform handle (HWND on Windows); for capture/interop
    uint32_t process_id = 0;
    std::string title;           // UTF-8
    std::string app_id;          // executable base name on Windows, app_id on Wayland
    std::string class_name;
    Rect frame;                  // visible frame, excluding invisible resize borders/shadows
    MonitorId monitor = kNoMonitor;
    uint32_t dpi = 96;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;     // covers its whole monitor without decoration
    bool resizable = true;
    // The host draws this window's frame (title bar, borders): xdg-decoration
    // negotiated server-side, or an X11 window that leaves its frame to the
    // window manager. Always false for the shell backends (the OS draws).
    bool decorated = false;

    bool operator==(const WindowSnapshot&) const = default;
};

// Bits for WindowChanged::changes.
namespace change {
inline constexpr uint32_t Geometry = 1u << 0;
inline constexpr uint32_t Title = 1u << 1;
inline constexpr uint32_t State = 1u << 2;  // minimized / maximized / fullscreen / decorated
inline constexpr uint32_t Monitor = 1u << 3;
}  // namespace change

// A window became manageable: visible, top-level, an application window.
struct WindowAdded {
    WindowSnapshot window;
};

// A previously added window is gone (destroyed, hidden by its app, or cloaked
// by the system). Never emitted for a window that was not added.
struct WindowRemoved {
    WindowId id = kNoWindow;
};

struct WindowChanged {
    WindowSnapshot window;
    uint32_t changes = 0;
};

// The system's active window changed. kNoWindow when focus moved to
// something the backend does not track (desktop, a tool window, the host).
struct FocusChanged {
    WindowId id = kNoWindow;
};

// The user started / finished an interactive move or resize of a window.
struct MoveSizeStarted {
    WindowId id = kNoWindow;
};
struct MoveSizeEnded {
    WindowId id = kNoWindow;
};

// Full replacement of the monitor set (connect/disconnect, resolution or DPI
// change, work-area change from a reservation or the taskbar).
struct MonitorsChanged {
    std::vector<MonitorSnapshot> monitors;
};

// The platform renegotiated an edge reservation; the host should move its
// panel window into `rect`.
struct ReservationChanged {
    ReservationId id = kNoReservation;
    MonitorId monitor = kNoMonitor;
    Rect rect;
};

using Event = std::variant<WindowAdded, WindowRemoved, WindowChanged, FocusChanged,
                           MoveSizeStarted, MoveSizeEnded, MonitorsChanged, ReservationChanged>;

}  // namespace brocompositor
