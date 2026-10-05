// What the Wayland server tells its host beyond the portable WM facts.
//
// The server role pushes two kinds of value snapshots:
//   * portable facts (WindowAdded, MonitorsChanged, ...) into
//     ServerBackend::events(), for the WindowManager core, exactly like the
//     Windows shell backend;
//   * server facts (ServerEvent, below) into ServerBackend::server_events():
//     output frame pacing and presentation, surface commits and trees, raw
//     input for the host's hit testing, cursor images, client requests.
//
// Coordinates: the layout space is wlroots' output layout (logical pixels).
// MonitorSnapshot::bounds and WindowSnapshot::frame are in it; an output's
// pixel size is bounds * scale.
#pragma once

#include "brocompositor/events.h"
#include "brocompositor/linux/input_events.h"
#include "brocompositor/linux/session_events.h"
#include "brocompositor/surface.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace brocompositor::wl {

using SurfaceId = uint64_t;
using LayerSurfaceId = uint64_t;
inline constexpr SurfaceId kNoSurface = 0;

// ---------------------------------------------------------------- outputs

struct OutputMode {
    Size size;                 // pixels
    int32_t refresh_mhz = 0;   // 0: unknown / free running
    bool preferred = false;
    bool operator==(const OutputMode&) const = default;
};

struct OutputInfo {
    MonitorId id = kNoMonitor;
    std::string name, description, make, model;
    bool enabled = false;
    Size pixel_size;           // current mode, untransformed
    int32_t refresh_mhz = 0;
    float scale = 1.0f;
    uint32_t transform = 0;    // wl_output_transform
    Rect layout;               // logical rectangle in the layout space
    Rect work_area;            // layout minus exclusive zones and host reservations
    std::vector<OutputMode> modes;
    uint64_t images_generation = 0;  // bumps when the presentable image set changes
    bool operator==(const OutputInfo&) const = default;
};

// ---------------------------------------------------------------- surfaces

// One drawable surface of a window or layer surface, bottom to top. Offsets
// are logical pixels relative to the window's frame origin (the xdg window
// geometry's top-left), so client-side shadows have negative offsets.
struct SurfaceNode {
    SurfaceId surface = kNoSurface;
    Point offset;
    Size size;            // logical size (after buffer scale / viewporter)
    bool popup = false;   // part of an xdg_popup (drawn above the window)
    bool operator==(const SurfaceNode&) const = default;
};

enum class Layer : uint32_t { Background = 0, Bottom = 1, Top = 2, Overlay = 3 };

enum class KeyboardInteractivity : uint32_t { None = 0, Exclusive = 1, OnDemand = 2 };

struct LayerSurfaceInfo {
    LayerSurfaceId id = 0;
    MonitorId monitor = kNoMonitor;
    Layer layer = Layer::Background;
    std::string name_space;          // layer_surface namespace ("panel", "wallpaper")
    Rect rect;                       // layout space
    int32_t exclusive_zone = 0;
    KeyboardInteractivity keyboard = KeyboardInteractivity::None;
    bool mapped = false;
    uint32_t process_id = 0;
    ReservationId reservation = kNoReservation;  // set while it holds an exclusive zone
    bool operator==(const LayerSurfaceInfo&) const = default;
};

// ---------------------------------------------------------------- events

// The output wants a new frame (vblank pacing). Render, then present_output().
struct OutputFrame {
    MonitorId output = kNoMonitor;
};

// A presented frame reached the screen (or was discarded: presented=false).
struct OutputPresented {
    MonitorId output = kNoMonitor;
    uint64_t image_id = 0;
    uint64_t commit_seq = 0;
    bool presented = false;
    int64_t timestamp_ns = 0;    // CLOCK_MONOTONIC
    uint32_t refresh_ns = 0;
};

// present_output() could not be committed; the image went back to the pool.
struct OutputPresentFailed {
    MonitorId output = kNoMonitor;
    uint64_t image_id = 0;
};

// Output added, removed or reconfigured (mode, scale, transform, position,
// image set). The full list replaces the previous one.
struct OutputsChanged {
    std::vector<OutputInfo> outputs;
};

// A surface committed new state. `window` / `layer` / `unmanaged` / `lock`
// name the tree it belongs to (0 when none: cursor, drag icon, unmapped).
struct SurfaceCommitted {
    SurfaceId surface = kNoSurface;
    WindowId window = kNoWindow;
    LayerSurfaceId layer = 0;
    UnmanagedId unmanaged = 0;
    MonitorId lock = kNoMonitor;  // a lock surface of this output
    bool new_buffer = false;
};

// The surface tree of a window changed (subsurface/popup added, removed,
// moved, restacked). Query window_surfaces().
struct WindowTreeChanged {
    WindowId window = kNoWindow;
};

struct LayerSurfaceAdded {
    LayerSurfaceInfo info;
};
struct LayerSurfaceChanged {
    LayerSurfaceInfo info;
};
struct LayerSurfaceRemoved {
    LayerSurfaceId id = 0;
};

// What to draw as the pointer: a client surface (with hotspot), a named
// cursor-shape (xcursor name) or nothing. Default after the pointer leaves
// client surfaces: shape "default".
struct CursorChanged {
    SurfaceId surface = kNoSurface;
    Point hotspot;
    std::string shape;
    bool hidden = false;
};

// A drag-and-drop icon appeared (draw it at the cursor) or went away.
struct DragIconChanged {
    SurfaceId surface = kNoSurface;  // kNoSurface: drag ended / no icon
};

// Where a raw input event came from. Client input (virtual-keyboard /
// virtual-pointer protocols) is synthesized by another program: hosts may
// want to keep it away from their own shortcuts, and the server keeps it
// away from everything while the session is locked (LockedVirtualInput).
enum class InputOrigin : uint32_t {
    Device = 0,  // a physical device (libinput, a nested backend's seat)
    Host = 1,    // ServerBackend::inject_*()
    Client = 2,  // a client's virtual keyboard / pointer; client_pid says whose
};

// Raw input, already accumulated into the server's cursor (clamped to the
// output layout). The host hit-tests and routes it back with the pointer_* /
// keyboard_* methods; nothing reaches a client unless the host routes it.
struct PointerMotion {
    uint32_t time_msec = 0;
    double x = 0, y = 0;     // cursor position, layout space
    double dx = 0, dy = 0;   // unaccelerated device delta (0 for absolute devices)
    InputOrigin origin = InputOrigin::Device;
    uint32_t client_pid = 0;  // InputOrigin::Client
};
struct PointerButton {
    uint32_t time_msec = 0;
    uint32_t button = 0;     // linux/input-event-codes.h (BTN_LEFT = 0x110)
    bool pressed = false;
    InputOrigin origin = InputOrigin::Device;
    uint32_t client_pid = 0;
};
struct PointerAxis {
    uint32_t time_msec = 0;
    uint32_t orientation = 0;  // 0 vertical, 1 horizontal (wl_pointer_axis)
    uint32_t source = 0;       // wl_pointer_axis_source
    double delta = 0;
    int32_t delta_discrete = 0;  // value120
    InputOrigin origin = InputOrigin::Device;
    uint32_t client_pid = 0;
};
struct PointerFrame {};
// Serialized xkb modifier state (what wl_keyboard.modifiers carries).
struct KeyboardModifiers {
    uint32_t depressed = 0, latched = 0, locked = 0, group = 0;
    bool operator==(const KeyboardModifiers&) const = default;
};

struct KeyboardKey {
    uint32_t time_msec = 0;
    uint32_t keycode = 0;    // evdev code (KEY_A = 30)
    bool pressed = false;
    uint32_t keysym = 0;     // xkb keysym after the server's keymap
    uint32_t modifiers = 0;  // xkb effective modifier mask (wlr_keyboard_modifier bits) before this key
    // Modifier state after this key. ServerBackend::keyboard_key() applies it
    // with the key, so clients see modifiers in order with the keys the host
    // routes; a host that swallows a key still forwards this with
    // keyboard_modifiers().
    KeyboardModifiers modifiers_after;
    // The focused surface inhibits the host's shortcuts (see
    // ShortcutsInhibitChanged).
    bool shortcuts_inhibited = false;
    InputOrigin origin = InputOrigin::Device;
    uint32_t client_pid = 0;  // InputOrigin::Client
};

enum class WindowRequestKind : uint32_t {
    Move = 0,
    Resize = 1,
    Maximize = 2,
    Unmaximize = 3,
    Fullscreen = 4,
    Unfullscreen = 5,
    Minimize = 6,
    ShowMenu = 7,
    Activate = 8,  // xdg-activation / _NET_ACTIVE_WINDOW / a taskbar: focus this window
    Close = 9,     // a taskbar asked to close the window (the host decides, then close())
    Unminimize = 10,
};

// A client asked for something the host decides (interactive move/resize,
// state changes). The protocol's mandatory configure reply is already sent.
// `foreign` requests come from another client (a taskbar using
// wlr-foreign-toplevel-management) rather than the window's own.
struct WindowRequest {
    WindowId window = kNoWindow;
    WindowRequestKind kind = WindowRequestKind::Move;
    uint32_t edges = 0;          // Resize: wlr_edges bits
    MonitorId monitor = kNoMonitor;  // Fullscreen: requested output
    Point point;                 // ShowMenu: window-relative
    bool foreign = false;
};

struct SelectionChanged {
    bool primary = false;
    std::vector<std::string> mime_types;  // empty: cleared
};

using ServerEvent =
    std::variant<OutputFrame, OutputPresented, OutputPresentFailed, OutputsChanged, SurfaceCommitted,
                 WindowTreeChanged, LayerSurfaceAdded, LayerSurfaceChanged, LayerSurfaceRemoved, CursorChanged,
                 DragIconChanged, PointerMotion, PointerButton, PointerAxis, PointerFrame, KeyboardKey,
                 WindowRequest, SelectionChanged,
                 // input_events.h
                 TouchDown, TouchMotion, TouchUp, TouchCancel, TouchFrame, TabletToolProximity,
                 TabletToolMotion, TabletToolTip, TabletToolButton, TabletPadButton, TabletPadRing,
                 TabletPadStrip, PointerConstraintChanged, ShortcutsInhibitChanged,
                 // session_events.h
                 XwaylandStatus, UnmanagedSurfaceAdded, UnmanagedSurfaceChanged, UnmanagedSurfaceRemoved,
                 SessionLockChanged, LockSurfaceChanged, IdleInhibitChanged, CaptureRequest, GammaChanged>;

}  // namespace brocompositor::wl
