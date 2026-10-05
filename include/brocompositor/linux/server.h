// Linux backend, server role: brocompositor is the Wayland display server.
//
// wlroots is used as a protocol / backend / input / session toolkit only:
// there is no wlr_renderer and no wlr_scene. The host renders every output
// itself (Vulkan in bro) from the client surfaces this server hands out as
// SurfaceSources, into images this server allocates per output, and gives
// each finished image back with present_output().
//
// Threads: one server thread owns the wl_display, the wlroots backend and
// every protocol object. Facts reach the host as value snapshots through two
// queues (events() for the portable WindowManager core, server_events() for
// everything else). Every method may be called from any thread; mutations are
// posted to the server thread (fire-and-forget unless they return a value, in
// which case they wait for the server thread to answer).
//
// Host frame loop:
//
//   for (auto& e : server->server_events().drain())
//       if (auto* f = std::get_if<wl::OutputFrame>(&e)) {
//           auto target = server->acquire_output_image(f->output);   // SharedImage
//           ... render the output's windows / layers / cursor into it,
//               leasing each client surface's newest frame (surface.h) ...
//           server->present_output(f->output, {target->id, damage, render_done_fd,
//                                              drawn_surfaces});
//       }
//   for (auto& e : server->events().drain()) server->execute(wm.handle(e));
//
// Input: devices feed the server's cursor and keymap; PointerMotion /
// PointerButton / KeyboardKey reach the host, which hit-tests its own scene
// (helped by hit_test()) and routes them with pointer_route() / pointer_button()
// / keyboard_key(). Keyboard focus follows FocusWindow commands.
#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/event_queue.h"
#include "brocompositor/linux/server_events.h"
#include "brocompositor/surface.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::wl {

// POSIX handles in the surface contract: -1 means none.
inline constexpr NativeHandle kNoFd{~uint64_t(0)};
inline int fd_of(NativeHandle h) { return h == kNoFd ? -1 : int(h.value); }
inline NativeHandle from_fd(int fd) { return fd < 0 ? kNoFd : NativeHandle{uint64_t(fd)}; }

using ServerEventQueue = MessageQueue<ServerEvent>;

enum class BackendKind : uint32_t {
    Auto = 0,      // wlroots autodetection: nested if WAYLAND_DISPLAY/DISPLAY is set, else DRM+libinput
    Headless = 1,  // no display, no input devices (virtual input only); outputs from the config
    Wayland = 2,   // nested: each output is a window of the parent Wayland compositor
    X11 = 3,       // nested: each output is an X11 window
    Drm = 4,       // DRM/KMS + libinput through a libseat session (a real seat / VT)
};

enum class OutputBufferKind : uint32_t {
    Auto = 0,    // dmabuf when the backend has a DRM device (GBM, else dumb buffers), else shm
    Shm = 1,     // memfd-backed wl_shm-style images (CPU rendering, any nested parent)
    DmaBuf = 2,  // GBM / dumb dmabufs on the backend's DRM device
};

struct DmabufFormat {
    uint32_t fourcc = 0;              // DRM_FORMAT_*
    std::vector<uint64_t> modifiers;  // DRM_FORMAT_MOD_*
};

struct ServerConfig {
    BackendKind backend = BackendKind::Auto;
    // Socket under XDG_RUNTIME_DIR; empty: the first free "wayland-N".
    std::string socket_name;

    // Headless / nested outputs created at startup.
    uint32_t initial_outputs = 1;
    Size initial_output_size{1280, 720};
    int32_t headless_refresh_mhz = 60000;
    float initial_scale = 1.0f;

    // DRM render node for linux-dmabuf feedback and GBM ("" : the backend's
    // device, else the first /dev/dri/renderD*). Without one, linux-dmabuf is
    // not offered and output images are shm.
    std::string render_node;
    // Formats + modifiers advertised to clients (what the host can import;
    // vk::Importer::dmabuf_formats() answers this). Empty: XRGB/ARGB/XBGR/ABGR
    // with the LINEAR modifier.
    std::vector<DmabufFormat> dmabuf_formats;

    OutputBufferKind output_buffers = OutputBufferKind::Auto;
    uint32_t output_image_count = 3;
    uint32_t output_format = 0x34325258;  // DRM_FORMAT_XRGB8888

    // Keymap for every keyboard (xkb rule names; empty = xkb defaults / env).
    std::string xkb_rules, xkb_model, xkb_layout, xkb_variant, xkb_options;
    int32_t repeat_rate = 25, repeat_delay = 600;

    bool prefer_server_side_decorations = true;  // xdg-decoration preference
    // Log level for wlroots (0 silent, 1 error, 2 info, 3 debug); overridden
    // by BROCOMPOSITOR_WLR_LOG.
    int wlr_log_level = 1;
};

struct SurfaceState {
    Size size;                  // logical size
    Size buffer_size;           // pixels of the attached buffer
    int32_t buffer_scale = 1;
    uint32_t transform = 0;     // wl_output_transform of the buffer
    bool has_source_crop = false;
    double src_x = 0, src_y = 0, src_width = 0, src_height = 0;  // viewporter source, buffer-local logical px
    bool opaque = false;
    bool mapped = false;
};

// Every wl_surface is a SurfaceSource: images are the client's buffers
// (DmaBuf planes + modifier, or ShmFd), imported once by id and leased per
// frame. A leased buffer is never released to the client, so it is never
// overwritten while the host samples it. Frame::sync_fd (when not kNoFd) is
// the buffer's implicit write fence exported as a sync_file: GPU-wait it
// before sampling. presented() sends the surface's frame callbacks.
class ClientSurface : public SurfaceSource {
public:
    virtual SurfaceId id() const = 0;
    virtual SurfaceState state() const = 0;
    // Sends frame callbacks and presentation feedback as presented on
    // `output` (kNoMonitor: frame callbacks only).
    virtual void presented_on(MonitorId output, int64_t timestamp_ns) = 0;
};

struct SurfaceHit {
    SurfaceId surface = kNoSurface;
    double sx = 0, sy = 0;  // surface-local
};

struct PresentRequest {
    uint64_t image_id = 0;
    std::vector<Rect> damage;              // output pixels; empty: everything
    NativeHandle render_done = kNoFd;      // sync_file the server waits on (ownership passes)
    std::vector<SurfaceId> surfaces;       // drawn this frame: frame callbacks + feedback
};

struct OutputConfig {
    std::optional<bool> enabled;
    std::optional<Size> mode;            // picks the closest advertised mode, else a custom mode
    std::optional<int32_t> refresh_mhz;
    std::optional<float> scale;
    std::optional<uint32_t> transform;
    std::optional<Point> position;       // layout space
};

class ServerBackend {
public:
    // Creates the display, backend and globals and starts the server thread.
    // On return the socket accepts clients and the initial outputs are
    // reported (MonitorsChanged + OutputsChanged queued).
    static std::unique_ptr<ServerBackend> create(const ServerConfig& config, std::string* error);
    ~ServerBackend();

    ServerBackend(const ServerBackend&) = delete;
    ServerBackend& operator=(const ServerBackend&) = delete;

    EventQueue& events();
    ServerEventQueue& server_events();

    std::string socket_name() const;   // WAYLAND_DISPLAY for clients
    std::string runtime_dir() const;   // XDG_RUNTIME_DIR the socket lives in
    AdapterId adapter() const;         // render node of linux-dmabuf / output images

    // ---- portable WM commands ----
    bool execute(const Command& command);
    size_t execute(const std::vector<Command>& commands);  // returns failures
    bool place(WindowId id, const Rect& frame);
    bool set_visible(WindowId id, bool visible);
    bool focus(WindowId id);
    bool close(WindowId id);

    // ---- windows ----
    std::optional<WindowSnapshot> query(WindowId id) const;
    std::vector<WindowId> windows() const;
    bool visible(WindowId id) const;
    // xdg-decoration negotiated server-side: the host draws the title bar /
    // border around the frame (WindowChanged with change::State on change).
    bool server_side_decoration(WindowId id) const;
    bool set_window_state(WindowId id, bool maximized, bool fullscreen);
    std::vector<SurfaceNode> window_surfaces(WindowId id) const;
    std::optional<SurfaceHit> hit_test(WindowId id, double wx, double wy) const;  // frame-relative

    // ---- layer shell ----
    std::vector<LayerSurfaceInfo> layer_surfaces() const;
    std::vector<SurfaceNode> layer_surface_tree(LayerSurfaceId id) const;  // relative to rect origin
    std::optional<SurfaceHit> hit_test_layer(LayerSurfaceId id, double lx, double ly) const;
    bool focus_layer_surface(LayerSurfaceId id);

    // ---- surfaces ----
    std::shared_ptr<ClientSurface> surface(SurfaceId id) const;
    CursorChanged cursor() const;

    // ---- outputs ----
    std::vector<MonitorSnapshot> monitors() const;
    std::vector<OutputInfo> outputs() const;
    std::vector<SharedImage> output_images(MonitorId output) const;
    // A free image of the output to render into (nullopt when none is free).
    std::optional<SharedImage> acquire_output_image(MonitorId output);
    // Shows an acquired image. Returns false (and frees the image) when the
    // output or image is unknown; commit failures arrive as OutputPresentFailed.
    bool present_output(MonitorId output, PresentRequest request);
    void discard_output_image(MonitorId output, uint64_t image_id);
    void schedule_frame(MonitorId output);
    bool configure_output(MonitorId output, const OutputConfig& config);
    // Creates another output on a headless / nested backend.
    MonitorId add_output(Size size);

    // Edge reservation (the host's own panels): shrinks the work area like a
    // layer-shell exclusive zone.
    ReservationId reserve_edge(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted);
    bool release_edge(ReservationId id);

    // ---- input: routing decided by the host ----
    // Moves pointer focus to `surface` (kNoSurface clears it) and sends
    // surface-local motion.
    void pointer_route(SurfaceId surface, double sx, double sy, uint32_t time_msec);
    void pointer_button(uint32_t time_msec, uint32_t button, bool pressed);
    // `discrete` is in value120 units (one wheel click = 120), 0 for smooth.
    void pointer_axis(uint32_t time_msec, uint32_t orientation, double delta, int32_t discrete, uint32_t source);
    void pointer_frame();
    // Sends the key to the focused surface, then `modifiers_after` (from the
    // KeyboardKey event) when it changed.
    void keyboard_key(uint32_t time_msec, uint32_t keycode, bool pressed, const KeyboardModifiers& modifiers_after);
    void keyboard_modifiers(const KeyboardModifiers& modifiers);
    void warp_cursor(double x, double y);
    std::pair<double, double> cursor_position() const;

    // ---- input: virtual devices (tests, remote input) ----
    // They feed the same path as real devices (server_events()).
    void inject_key(uint32_t keycode, bool pressed);
    void inject_pointer_motion(double dx, double dy);
    void inject_pointer_warp(double x, double y);
    void inject_pointer_button(uint32_t button, bool pressed);
    void inject_pointer_axis(uint32_t orientation, double delta, int32_t discrete);  // discrete: value120

    struct Impl;

private:
    explicit ServerBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace brocompositor::wl
