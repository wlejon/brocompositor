// Server-thread records for the desktop-session features: XWayland windows,
// taskbar handles, touch / tablet devices, pointer constraints, text input,
// the session lock, screen capture jobs and gamma controls. Included by
// server_impl.h; everything here belongs to the server thread except
// LockGate, which ClientSurfaceImpl reads from host threads.
#pragma once

#include "brocompositor/linux/server.h"
#include "linux/wlr.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace brocompositor::wl {

struct Server;
struct OutputImageSlot;

// ---------------------------------------------------------------- session lock gate

// What host threads may see while the session is locked: only the surfaces
// of the lock client's lock-surface trees.
struct LockGate {
    std::atomic<bool> locked{false};
    mutable std::mutex m;
    std::set<SurfaceId> allowed;
    bool allows(SurfaceId id) const {
        if (!locked.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(m);
        return allowed.count(id) != 0;
    }
};

// ---------------------------------------------------------------- taskbar handles

// wlr-foreign-toplevel-management + ext-foreign-toplevel-list handles of
// one mapped window (xdg or X11).
struct ForeignHandles {
    WindowId window = kNoWindow;
    wlr_foreign_toplevel_handle_v1* wlr = nullptr;
    wlr_ext_foreign_toplevel_handle_v1* ext = nullptr;
    std::set<wlr_output*> outputs;
    std::string title, app_id;
    WindowId parent = kNoWindow;
    bool maximized = false, minimized = false, activated = false, fullscreen = false;
    Listener req_maximize, req_minimize, req_activate, req_fullscreen, req_close, wlr_destroy;
};

// ---------------------------------------------------------------- XWayland

struct XwaylandRec {
    Server* srv = nullptr;
#ifdef BC_HAVE_XWAYLAND
    wlr_xwayland_surface* xs = nullptr;
#endif
    WindowId id = kNoWindow;     // managed window, minted on map
    UnmanagedId unmanaged = 0;   // override-redirect surface, minted on map
    bool visible = true;
    bool positioned = false;
    bool activated = false;
    bool minimized = false;
    WindowSnapshot snap;
    UnmanagedSurfaceInfo uinfo;
    std::set<wlr_output*> entered;
    std::unique_ptr<ForeignHandles> foreign;
    Listener destroy, associate, dissociate, map, unmap, commit;
    Listener req_configure, req_move, req_resize, req_minimize, req_maximize, req_fullscreen, req_activate;
    Listener set_title, set_class, set_parent, set_override_redirect, set_geometry, set_decorations;
};

// ---------------------------------------------------------------- touch / tablets

// Kinds for Server::inject_touch / inject_tablet / tablet_pad_event.
enum InjectTouchKind : int { kInjectDown = 0, kInjectMotion = 1, kInjectUp = 2, kInjectFrame = 3 };
enum InjectTabletKind : int {
    kInjectProximity = 0, kInjectToolMotion = 1, kInjectTip = 2, kInjectToolButton = 3, kInjectPadButton = 4,
};
enum PadEventKind : int { kPadButton = 0, kPadRing = 1, kPadStrip = 2 };

struct TouchRec {
    Server* srv = nullptr;
    wlr_touch* touch = nullptr;
    bool is_virtual = false;
    Listener down, up, motion, cancel, frame, destroy;
};

struct TabletRec {
    Server* srv = nullptr;
    wlr_tablet* tablet = nullptr;
    wlr_tablet_v2_tablet* v2 = nullptr;
    Listener axis, proximity, tip, button, destroy;
};

struct TabletToolRec {
    TabletToolId id = 0;
    wlr_tablet_tool* tool = nullptr;
    wlr_tablet_v2_tablet_tool* v2 = nullptr;
    TabletRec* tablet = nullptr;  // the tablet it was last seen on
    double x = 0, y = 0;          // layout space
    Listener destroy;
};

struct TabletPadRec {
    Server* srv = nullptr;
    TabletPadId id = 0;
    wlr_tablet_pad* pad = nullptr;
    wlr_tablet_v2_tablet_pad* v2 = nullptr;
    TabletRec* tablet = nullptr;
    wlr_surface* focus = nullptr;
    Listener button, ring, strip, attach, destroy;
};

// ---------------------------------------------------------------- pointer / keyboard extras

struct ConstraintRec {
    wlr_pointer_constraint_v1* constraint = nullptr;
    Listener set_region, destroy;
};

struct ShortcutsInhibitorRec {
    wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = nullptr;
    Listener destroy;
};

struct IdleInhibitorRec {
    wlr_idle_inhibitor_v1* inhibitor = nullptr;
    Listener destroy;
};

// ---------------------------------------------------------------- text input

struct TextInputRec {
    wlr_text_input_v3* ti = nullptr;
    Listener enable, commit, disable, destroy;
};

struct InputPopupRec {
    wlr_input_popup_surface_v2* popup = nullptr;
    Listener map, unmap, destroy;
};

// ---------------------------------------------------------------- session lock

struct LockSurfaceRec {
    wlr_session_lock_surface_v1* ls = nullptr;
    MonitorId output = kNoMonitor;
    Listener destroy, map, unmap;
};

// ---------------------------------------------------------------- capture

// One copy into a client buffer (wlr-screencopy frame or
// ext-image-copy-capture frame), from an output image or a window.
struct CaptureJob {
    uint64_t id = 0;
    MonitorId output = kNoMonitor;
    WindowId window = kNoWindow;
    Rect region;          // output pixels (output source)
    float scale = 1.0f;   // window source
    bool cursor = false;
    wlr_buffer* buffer = nullptr;  // locked while the job lives
    std::vector<int> fds;          // dup()ed fds of `target` while the host copies
    SharedImage target;
    std::vector<Rect> damage;      // reported to the client with the result
    int64_t when_ns = 0;
    // Called once with the outcome (the job is destroyed right after).
    std::function<void(CaptureJob&, bool ok)> finish;
    Listener buffer_destroy;
};

// Waits for the next present of an output: called after each successful
// commit with the presented image and its damage (output pixels, empty =
// everything). Returns true when it is done waiting.
using PresentWaiter = std::function<bool(OutputImageSlot* image, const std::vector<Rect>& damage, int64_t when_ns)>;

// ---------------------------------------------------------------- gamma

struct GammaState {
    wl_resource* control = nullptr;   // the client's zwlr_gamma_control_v1 (one per output)
    uint32_t size = 0;
    bool hardware = false;
    std::vector<uint16_t> ramps;      // r, g, b (empty: identity)
    bool pending = false;             // hardware: apply with the next commit
};

}  // namespace brocompositor::wl
