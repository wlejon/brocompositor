// Server-role facts for a desktop session: XWayland, unmanaged (X11
// override-redirect) surfaces, the session lock, idle state, screen capture
// requests and gamma tables.
#pragma once

#include "brocompositor/events.h"
#include "brocompositor/surface.h"

#include <cstdint>
#include <string>
#include <vector>

namespace brocompositor::wl {

using SurfaceId = uint64_t;

// ---------------------------------------------------------------- XWayland

// XWayland is listening (`display` is the DISPLAY value for X11 clients) or
// is gone (empty `display`). With XwaylandMode::Lazy the X server itself
// only starts when the first X11 client connects; the display is usable as
// soon as this event arrives.
struct XwaylandStatus {
    std::string display;
    bool running = false;  // the X server process is up (false while lazily waiting)
};

// ---------------------------------------------------------------- unmanaged surfaces

using UnmanagedId = uint64_t;

// An X11 override-redirect window (menus, tooltips, drag icons, splash
// screens): not a managed window, positioned by its client in layout space.
// The host draws mapped ones above every window, in list order (bottom to
// top), and hit-tests them first.
struct UnmanagedSurfaceInfo {
    UnmanagedId id = 0;
    SurfaceId surface = 0;
    Rect rect;                       // layout space
    WindowId owner = kNoWindow;      // transient-for / same-client window, when known
    uint32_t process_id = 0;
    bool operator==(const UnmanagedSurfaceInfo&) const = default;
};

struct UnmanagedSurfaceAdded {
    UnmanagedSurfaceInfo info;
};
struct UnmanagedSurfaceChanged {
    UnmanagedSurfaceInfo info;
};
struct UnmanagedSurfaceRemoved {
    UnmanagedId id = 0;
};

// ---------------------------------------------------------------- session lock

enum class LockState : uint32_t {
    Unlocked = 0,
    Locked = 1,     // a lock client (ext-session-lock-v1) holds the lock
    Abandoned = 2,  // the lock client died without unlocking: still locked until a new lock client unlocks
};

// The session lock changed. While not Unlocked the server withholds every
// client surface except the lock surfaces: window / layer / unmanaged trees
// are empty, surface() and acquire() refuse other surfaces, hit tests miss,
// and pointer, keyboard, touch and tablet input (including popup grabs and
// input-method grabs) reach only lock surfaces. The host draws only the
// lock surfaces (lock_surface_tree) and its own lock background.
struct SessionLockChanged {
    LockState state = LockState::Unlocked;
};

// The lock surface of an output appeared, changed or went away
// (surface == 0). Its tree is relative to the output's layout origin.
struct LockSurfaceChanged {
    MonitorId output = kNoMonitor;
    SurfaceId surface = 0;
};

// ---------------------------------------------------------------- idle

// Whether some visible surface inhibits idling (zwp_idle_inhibit_v1, e.g. a
// video player). The server applies it to ext-idle-notify itself; hosts use
// it for their own blanking / DPMS.
struct IdleInhibitChanged {
    bool inhibited = false;
};

// ---------------------------------------------------------------- capture

// A screen-capture client (wlr-screencopy, ext-image-copy-capture) needs a
// copy the server cannot make on the CPU (a tiled output image, a dmabuf
// client buffer, a window source, or ServerConfig::host_capture_copies).
// The host copies and answers with ServerBackend::capture_done().
//
//   * output capture (window == kNoWindow): copy `region` (pixels) of the
//     output image `source_image` (just presented; render into that image
//     again only after this copy was submitted) to the top-left of `target`;
//   * window capture: render the window's surface tree (window_surfaces()),
//     frame origin at the top-left, at `scale`, into `target`.
//
// `target` is the client's buffer (DmaBuf or ShmFd); its fds stay valid
// until capture_done().
struct CaptureRequest {
    uint64_t id = 0;
    MonitorId output = kNoMonitor;
    uint64_t source_image = 0;
    Rect region;
    WindowId window = kNoWindow;
    float scale = 1.0f;
    bool with_cursor = false;  // the client asked for the cursor to be included
    SharedImage target;
};

// ---------------------------------------------------------------- gamma

// A gamma-control client (wlr-gamma-control, e.g. wlsunset) set an output's
// gamma ramps, or they were restored (empty ramps). `ramps` holds the red,
// green and blue ramps back to back, `size` entries each. When `hardware`
// is true the server already applies them through KMS; otherwise the host
// applies them when it composites the output.
struct GammaChanged {
    MonitorId output = kNoMonitor;
    uint32_t size = 0;
    std::vector<uint16_t> ramps;
    bool hardware = false;
};

}  // namespace brocompositor::wl
