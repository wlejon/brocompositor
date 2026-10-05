// Server-role input facts beyond pointer and keyboard: touch, tablets (tools
// and pads), pointer constraints and keyboard-shortcut inhibition. Like
// pointer and keyboard input, raw touch and tablet input reaches the host,
// which hit-tests and routes it back (ServerBackend::touch_* / tablet_*);
// nothing reaches a client unless the host routes it.
#pragma once

#include "brocompositor/geometry.h"

#include <cstdint>
#include <string>

namespace brocompositor::wl {

using SurfaceId = uint64_t;

// ---------------------------------------------------------------- touch

// Touch points in layout space (devices are mapped onto their output, else
// onto the whole layout). `id` is the device's slot, unique while down.
struct TouchDown {
    uint32_t time_msec = 0;
    int32_t id = 0;
    double x = 0, y = 0;
};
struct TouchMotion {
    uint32_t time_msec = 0;
    int32_t id = 0;
    double x = 0, y = 0;
};
struct TouchUp {
    uint32_t time_msec = 0;
    int32_t id = 0;
};
struct TouchCancel {
    uint32_t time_msec = 0;
    int32_t id = 0;
};
struct TouchFrame {};

// ---------------------------------------------------------------- tablets

using TabletToolId = uint64_t;  // one physical tool (pen, eraser, ...)
using TabletPadId = uint64_t;

enum class TabletToolType : uint32_t {
    Pen = 1, Eraser = 2, Brush = 3, Pencil = 4, Airbrush = 5, Mouse = 6, Lens = 7, Totem = 8,
};

// Axis values of a tool sample; `axes` says which ones changed.
struct TabletToolAxes {
    enum : uint32_t {
        Pressure = 1u << 0, Distance = 1u << 1, Tilt = 1u << 2, Rotation = 1u << 3, Slider = 1u << 4,
        Wheel = 1u << 5,
    };
    uint32_t axes = 0;
    double pressure = 0;     // 0..1
    double distance = 0;     // 0..1
    double tilt_x = 0, tilt_y = 0;  // degrees
    double rotation = 0;     // degrees
    double slider = 0;       // -1..1
    double wheel_degrees = 0;
    int32_t wheel_clicks = 0;
};

struct TabletToolProximity {
    uint32_t time_msec = 0;
    TabletToolId tool = 0;
    TabletToolType type = TabletToolType::Pen;
    bool in = false;
    double x = 0, y = 0;  // layout space
};
// Position (layout space) and the axes that changed.
struct TabletToolMotion {
    uint32_t time_msec = 0;
    TabletToolId tool = 0;
    double x = 0, y = 0;
    TabletToolAxes axes;
};
struct TabletToolTip {
    uint32_t time_msec = 0;
    TabletToolId tool = 0;
    bool down = false;
};
struct TabletToolButton {
    uint32_t time_msec = 0;
    TabletToolId tool = 0;
    uint32_t button = 0;  // BTN_STYLUS, BTN_STYLUS2, ...
    bool pressed = false;
};
struct TabletPadButton {
    uint32_t time_msec = 0;
    TabletPadId pad = 0;
    uint32_t button = 0;  // pad button index
    bool pressed = false;
};
struct TabletPadRing {
    uint32_t time_msec = 0;
    TabletPadId pad = 0;
    uint32_t ring = 0;
    double position = 0;  // degrees, -1 when the finger lifted
    bool finger = false;
};
struct TabletPadStrip {
    uint32_t time_msec = 0;
    TabletPadId pad = 0;
    uint32_t strip = 0;
    double position = 0;  // 0..1, -1 when the finger lifted
    bool finger = false;
};

// ---------------------------------------------------------------- pointer constraints

enum class PointerConstraintKind : uint32_t { None = 0, Locked = 1, Confined = 2 };

// A pointer constraint (zwp_pointer_constraints_v1) became active on the
// pointer-focused surface, or the active one ended (kind None). The server
// enforces it: a locked pointer does not move the cursor (PointerMotion
// still carries dx/dy and relative motion reaches the client), a confined
// one is clamped to the surface's region. Hosts typically hide the cursor
// while Locked.
struct PointerConstraintChanged {
    SurfaceId surface = 0;
    PointerConstraintKind kind = PointerConstraintKind::None;
};

// ---------------------------------------------------------------- keyboard shortcuts

// A surface holding the keyboard focus asked the host not to act on its
// keyboard shortcuts (zwp_keyboard_shortcuts_inhibit_v1, e.g. a VM viewer or
// a remote desktop), or that inhibition ended. KeyboardKey carries the
// current state too; the host keeps a way out (the protocol allows it).
struct ShortcutsInhibitChanged {
    SurfaceId surface = 0;
    bool active = false;
};

}  // namespace brocompositor::wl
