// Internal structure of the Wayland server role. Everything named *Rec, and
// every wlroots pointer, belongs to the server thread. The host-visible
// mirror (Mirror) and ClientSurfaceImpl are the only state other threads
// touch, each under its own mutex.
#pragma once

#include "brocompositor/linux/server.h"
#include "linux/dispatcher.h"
#include "linux/wlr.h"

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct gbm_device;

namespace brocompositor::wl {

struct Server;
class ClientSurfaceImpl;
struct OutputImageSlot;

uint64_t next_image_id();  // process-unique SharedImage ids

// ---------------------------------------------------------------- surfaces

struct SurfaceRec {
    Server* srv = nullptr;
    wlr_surface* surface = nullptr;
    SurfaceId id = kNoSurface;
    std::shared_ptr<ClientSurfaceImpl> source;
    Listener commit, destroy, new_subsurface;
};

// ---------------------------------------------------------------- windows

struct ToplevelRec {
    Server* srv = nullptr;
    wlr_xdg_toplevel* xdg = nullptr;
    WindowId id = kNoWindow;  // minted on map, 0 while unmapped
    Point pos;                // frame origin in layout space
    bool positioned = false;  // pos was chosen (initial placement or PlaceWindow)
    bool visible = true;
    Size last_geometry;
    WindowSnapshot snap;
    std::set<wlr_output*> entered;
    wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
    bool ssd = false;  // negotiated server-side decoration: the host draws the frame
    Listener map, unmap, commit, destroy;
    Listener req_move, req_resize, req_maximize, req_fullscreen, req_minimize, req_menu;
    Listener set_title, set_app_id, set_parent;
    Listener deco_request, deco_destroy;
};

struct PopupRec {
    Server* srv = nullptr;
    wlr_xdg_popup* xdg = nullptr;
    Listener commit, destroy, reposition;
};

// ---------------------------------------------------------------- layer shell

struct LayerRec {
    Server* srv = nullptr;
    wlr_layer_surface_v1* layer = nullptr;
    LayerSurfaceId id = 0;
    ReservationId reservation = kNoReservation;
    Rect strip;  // reserved by the exclusive zone
    Rect rect;
    bool mapped = false;
    bool announced = false;
    LayerSurfaceInfo last;
    Listener map, unmap, commit, destroy, new_popup, output_destroy;
};

// ---------------------------------------------------------------- outputs

struct HostReservation {
    ReservationId id = kNoReservation;
    MonitorId monitor = kNoMonitor;
    Edge edge = Edge::Top;
    int32_t thickness = 0;
    Rect rect;
};

enum class SlotState : uint32_t { Free, Host, Queued, Scanout };

struct OutputRec {
    Server* srv = nullptr;
    wlr_output* output = nullptr;
    MonitorId id = kNoMonitor;
    Rect work_area;
    uint64_t images_generation = 0;
    // Current image set. Slots are wlr_buffers: dropping one frees it once the
    // backend no longer scans it out.
    std::vector<OutputImageSlot*> slots;
    bool use_shm = false;         // fell back to shm images
    uint64_t pending_image = 0;   // committed, waiting for present
    std::map<uint32_t, uint64_t> inflight;  // commit_seq -> image id
    Listener frame, present, destroy, request_state, commit;
};

// ---------------------------------------------------------------- input

struct KeyboardRec {
    Server* srv = nullptr;
    wlr_keyboard* keyboard = nullptr;
    bool is_virtual = false;
    // Mirrors the device's xkb state one key ahead, so a KeyboardKey event
    // can carry the modifier state that follows it.
    xkb_state* shadow = nullptr;
    Listener key, destroy;
    ~KeyboardRec() {
        if (shadow) xkb_state_unref(shadow);
    }
};

struct PointerRec {
    Server* srv = nullptr;
    wlr_pointer* pointer = nullptr;
    Listener motion, motion_abs, button, axis, frame, destroy;
};

// ---------------------------------------------------------------- host mirror

struct WindowMirror {
    WindowSnapshot snap;
    bool visible = true;
    bool ssd = false;
    SurfaceId root = kNoSurface;
    std::vector<SurfaceNode> tree;
};

struct LayerMirror {
    LayerSurfaceInfo info;
    std::vector<SurfaceNode> tree;
};

struct OutputMirror {
    OutputInfo info;
    struct Image {
        SharedImage desc;
        SlotState state = SlotState::Free;
    };
    std::vector<Image> images;
};

struct Mirror {
    mutable std::mutex m;
    std::map<WindowId, WindowMirror> windows;
    std::map<LayerSurfaceId, LayerMirror> layers;
    std::map<SurfaceId, std::shared_ptr<ClientSurfaceImpl>> surfaces;
    std::map<MonitorId, OutputMirror> outputs;
    std::vector<MonitorSnapshot> monitors;
    CursorChanged cursor{kNoSurface, {}, "default", false};
    double cursor_x = 0, cursor_y = 0;
};

// ---------------------------------------------------------------- server

struct Server {
    ServerConfig config;
    EventQueue events;
    ServerEventQueue server_events;
    std::shared_ptr<Dispatcher> dispatcher;
    Mirror mirror;
    std::thread thread;
    std::string socket;
    std::string runtime_dir;
    AdapterId adapter;
    std::atomic<bool> running{false};

    // ---- wlroots objects (server thread) ----
    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;
    wlr_backend* backend = nullptr;
    wlr_session* session = nullptr;
    wlr_compositor* compositor = nullptr;
    wlr_subcompositor* subcompositor = nullptr;
    wlr_xdg_shell* xdg_shell = nullptr;
    wlr_layer_shell_v1* layer_shell = nullptr;
    wlr_output_layout* layout = nullptr;
    wlr_seat* seat = nullptr;
    double cursor_x = 0, cursor_y = 0;  // layout space
    wlr_presentation* presentation = nullptr;
    wlr_linux_dmabuf_v1* linux_dmabuf = nullptr;
    wlr_output_manager_v1* output_manager = nullptr;
    wlr_xdg_decoration_manager_v1* decoration_manager = nullptr;
    wlr_xdg_activation_v1* activation = nullptr;
    wlr_cursor_shape_manager_v1* cursor_shape = nullptr;
    wlr_drm_format_set dmabuf_formats{};
    int render_fd = -1;          // render node for GBM output images (owned)
    std::string render_node_path;
    ::gbm_device* gbm = nullptr;          // on render_fd
    ::gbm_device* backend_gbm = nullptr;  // on the backend's DRM fd (KMS card / nested parent node)

    Listener new_output, new_input, new_surface, new_toplevel, new_popup, new_layer_surface;
    Listener new_decoration, request_activate, request_cursor_shape;
    Listener seat_request_cursor, seat_request_selection, seat_request_primary, seat_request_drag,
        seat_start_drag;
    Listener output_mgr_apply, output_mgr_test, layout_change;
    Listener selection_changed, primary_changed;
    Listener drag_icon_destroy;

    std::unordered_map<wlr_surface*, std::unique_ptr<SurfaceRec>> surfaces;
    std::unordered_map<wlr_xdg_toplevel*, std::unique_ptr<ToplevelRec>> toplevels;
    std::unordered_map<wlr_xdg_popup*, std::unique_ptr<PopupRec>> popups;
    std::unordered_map<wlr_layer_surface_v1*, std::unique_ptr<LayerRec>> layers;
    std::map<MonitorId, std::unique_ptr<OutputRec>> outputs;
    std::list<std::unique_ptr<KeyboardRec>> keyboards;
    std::list<std::unique_ptr<PointerRec>> pointers;
    std::map<ReservationId, HostReservation> host_reservations;

    // Virtual input devices (always present, so the seat advertises a keyboard
    // and a pointer before any hardware shows up).
    wlr_keyboard* vkeyboard = nullptr;
    wlr_pointer* vpointer = nullptr;

    WindowId next_window = 1;
    MonitorId next_monitor = 1;
    SurfaceId next_surface = 1;
    LayerSurfaceId next_layer = 1;
    ReservationId next_reservation = 1;
    WindowId focused_window = kNoWindow;
    wlr_surface* focused_layer = nullptr;
    wlr_surface* pointer_surface = nullptr;
    wlr_surface* drag_icon = nullptr;

    std::set<wlr_surface*> dirty_roots;
    bool all_trees_dirty = false;
    wl_event_source* tree_idle = nullptr;
    bool outputs_dirty = false;
    wl_event_source* outputs_idle = nullptr;

    // ---- setup.cpp ----
    bool init(std::string* error);
    bool init_backend(std::string* error);
    bool init_globals(std::string* error);
    void init_dmabuf();
    void shutdown();

    // ---- surfaces.cpp ----
    void on_new_surface(wlr_surface* surface);
    SurfaceRec* rec(wlr_surface* surface);
    SurfaceId surface_id(wlr_surface* surface);
    void mark_tree_dirty(wlr_surface* surface);
    void mark_all_trees_dirty();
    void refresh_trees();
    // Root of the tree a surface belongs to: a toplevel or a layer surface.
    void resolve_root(wlr_surface* surface, ToplevelRec** top, LayerRec** layer);
    std::vector<SurfaceNode> build_tree(wlr_surface* root, Point origin_offset, wlr_xdg_surface* xdg_root,
                                        wlr_layer_surface_v1* layer_root);
    void send_frame_done(wlr_surface* surface, int64_t timestamp_ns);

    // ---- xdg.cpp ----
    void on_new_toplevel(wlr_xdg_toplevel* toplevel);
    void on_new_popup(wlr_xdg_popup* popup);
    void on_new_decoration(wlr_xdg_toplevel_decoration_v1* decoration);
    void apply_decoration(ToplevelRec& t);
    void on_activation_request(wlr_xdg_activation_v1_request_activate_event* event);
    ToplevelRec* toplevel(WindowId id);
    WindowSnapshot snapshot(ToplevelRec& t);
    void publish_window(ToplevelRec& t, uint32_t changes);
    void update_window_outputs(ToplevelRec& t);
    bool place_window(WindowId id, const Rect& frame);
    bool set_window_visible(WindowId id, bool visible);
    bool close_window(WindowId id);
    bool set_window_state(WindowId id, bool maximized, bool fullscreen);
    Point root_origin(ToplevelRec& t);  // layout position of the root surface's origin

    // ---- layers.cpp ----
    void on_new_layer_surface(wlr_layer_surface_v1* layer);
    void arrange_output(OutputRec& out);
    void arrange_all();
    LayerSurfaceInfo layer_info(LayerRec& l);
    LayerRec* layer_by_id(LayerSurfaceId id);

    // ---- outputs.cpp ----
    void on_new_output(wlr_output* output);
    OutputRec* output_rec(MonitorId id);
    OutputRec* output_rec(wlr_output* output);
    MonitorId add_output(Size size);
    bool configure_output(MonitorId id, const OutputConfig& config);
    bool apply_output_state(OutputRec& out, wlr_output_state* state);
    void mark_outputs_dirty();
    void publish_outputs();
    OutputInfo output_info(OutputRec& out);
    wlr_output* output_at(double x, double y);
    void present(MonitorId id, PresentRequest request);
    ReservationId reserve_edge(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted);
    bool release_edge(ReservationId id);
    void update_output_manager();

    // ---- output_images.cpp ----
    bool ensure_output_images(OutputRec& out, int width, int height);
    void free_output_images(OutputRec& out);
    void publish_output_images(OutputRec& out);
    OutputImageSlot* slot(OutputRec& out, uint64_t image_id);
    void set_image_state(MonitorId output, uint64_t image_id, SlotState state);

    // ---- seat.cpp ----
    void init_seat();
    void on_new_input(wlr_input_device* device);
    void add_keyboard(wlr_keyboard* keyboard, bool is_virtual);
    void add_pointer(wlr_pointer* pointer);
    void update_capabilities();
    void focus_window(WindowId id);
    void focus_surface(wlr_surface* surface);
    void pointer_route(SurfaceId surface, double sx, double sy, uint32_t time);
    void set_cursor(const CursorChanged& cursor);
    void forget_surface_focus(wlr_surface* surface);
    void inject_key(uint32_t keycode, bool pressed);
    void inject_pointer_motion(double dx, double dy);
    void inject_pointer_warp(double x, double y);
    void inject_pointer_button(uint32_t button, bool pressed);
    void inject_pointer_axis(uint32_t orientation, double delta, int32_t discrete);
    void publish_cursor_position();
    void warp(double x, double y);
    // Sends wl_keyboard.modifiers to the focused client when they differ from
    // what it last saw.
    void send_modifiers(const KeyboardModifiers& m);
    KeyboardModifiers sent_modifiers;

    // ---- selection.cpp ----
    void init_selection();
};

}  // namespace brocompositor::wl
