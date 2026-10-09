// Internal structure of the Wayland server role. Everything named *Rec, and
// every wlroots pointer, belongs to the server thread. The host-visible
// mirror (Mirror) and ClientSurfaceImpl are the only state other threads
// touch, each under its own mutex.
#pragma once

#include "brocompositor/linux/server.h"
#include "linux/desktop_records.h"
#include "linux/dispatcher.h"
#include "linux/wlr.h"

#include <atomic>
#include <deque>
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
    int64_t last_frame_done_ns = 0;  // CLOCK_MONOTONIC; when its frame callbacks were last answered
};

// ---------------------------------------------------------------- windows

// An xdg_toplevel_icon_v1 as set on a toplevel: immutable once set.
struct IconSet {
    std::string name;
    std::vector<WindowIcon> images;  // one per size (largest scale kept)
};

struct ToplevelRec {
    Server* srv = nullptr;
    std::shared_ptr<const IconSet> icon;  // xdg-toplevel-icon; null: none
    uint64_t icon_serial = 0;
    std::shared_ptr<const IconSet> pending_icon;  // set_icon, applied on the next commit
    bool icon_pending = false;
    wlr_xdg_toplevel* xdg = nullptr;
    WindowId id = kNoWindow;  // minted on map, 0 while unmapped
    Point pos;                // frame origin in layout space
    bool positioned = false;  // pos was chosen (initial placement or PlaceWindow)
    // Placements that also resize, waiting for the client: each origin takes
    // effect with the commit that acks its configure, so the window moves in
    // the same frame it takes the new size (never the old size at the new
    // place, then the new size). Oldest first.
    std::vector<std::pair<uint32_t, Point>> pending_pos;
    bool visible = true;
    Size last_geometry;
    WindowSnapshot snap;
    std::set<wlr_output*> entered;
    wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
    bool ssd = false;  // negotiated server-side decoration: the host draws the frame
    bool activated = false;
    bool minimized = false;
    std::unique_ptr<ForeignHandles> foreign;
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
    // commit_seq -> the presentation feedback of the client frames it drew,
    // sent when that commit is presented (discarded when it is not).
    std::map<uint32_t, std::vector<wlr_presentation_feedback*>> inflight_feedback;
    // Last successfully committed image. The server holds a lock on it, so it
    // stays out of the host's free list while captures copy from it.
    uint64_t front_image = 0;
    std::vector<PresentWaiter> present_waiters;
    GammaState gamma;
    Listener frame, present, destroy, request_state, commit;
};

// ---------------------------------------------------------------- input

struct KeyboardRec {
    Server* srv = nullptr;
    wlr_keyboard* keyboard = nullptr;
    bool is_virtual = false;
    wl_client* owner = nullptr;  // virtual-keyboard-v1 client (IME loop detection, lock policy)
    uint32_t owner_pid = 0;
    // Mirrors the device's xkb state one key ahead, so a KeyboardKey event
    // can carry the modifier state that follows it.
    xkb_state* shadow = nullptr;
    Listener key, destroy, keymap;
    ~KeyboardRec() {
        if (shadow) xkb_state_unref(shadow);
    }
};

struct PointerRec {
    Server* srv = nullptr;
    wlr_pointer* pointer = nullptr;
    wlr_output* mapped_output = nullptr;  // absolute motion maps onto this output (else the layout)
    InputOrigin origin = InputOrigin::Device;
    wl_client* owner = nullptr;  // virtual-pointer-v1 client
    uint32_t owner_pid = 0;
    Listener motion, motion_abs, button, axis, frame, destroy;
};

uint32_t client_pid(wl_client* client);

// A key or button a client's virtual device produced, remembered briefly so
// that one the host routes after the session locked is dropped.
struct ClientInputStamp {
    uint32_t time_msec = 0;
    uint32_t code = 0;
    bool pressed = false;
    bool button = false;
    uint32_t pid = 0;
};

// A window of either shell: an xdg_toplevel or a managed X11 window.
struct WindowRef {
    ToplevelRec* xdg = nullptr;
    XwaylandRec* x = nullptr;
    explicit operator bool() const { return xdg || x; }
    bool operator==(const WindowRef&) const = default;
};

// The tree a surface belongs to.
struct RootRef {
    ToplevelRec* top = nullptr;
    LayerRec* layer = nullptr;
    XwaylandRec* x = nullptr;  // managed window or unmanaged surface
    LockSurfaceRec* lock = nullptr;
    WindowId window() const;
};

// ---------------------------------------------------------------- host mirror

struct WindowMirror {
    WindowSnapshot snap;
    std::shared_ptr<const IconSet> icon;
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
    std::optional<SurfaceNode> drag_icon;  // offset: against the pointer
    std::vector<UnmanagedSurfaceInfo> unmanaged;  // mapped, bottom to top
    std::string xwayland_display;
    LockState lock_state = LockState::Unlocked;
    std::map<MonitorId, std::vector<SurfaceNode>> lock_trees;
    std::map<MonitorId, GammaChanged> gamma;
    bool idle_inhibited = false;
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
    Listener drag_icon_destroy, drag_icon_commit;

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
    Point drag_icon_offset;  // the icon's top-left against the pointer

    std::set<wlr_surface*> dirty_roots;
    bool all_trees_dirty = false;
    wl_event_source* tree_idle = nullptr;
    bool outputs_dirty = false;
    wl_event_source* outputs_idle = nullptr;
    std::unordered_map<SurfaceId, wlr_surface*> surface_ids;
    // Frame callbacks for surfaces nothing draws (hidden, minimized, on
    // another workspace, covered): see frame_keepalive in surfaces.cpp.
    wl_event_source* frame_keepalive_timer = nullptr;
    void init_frame_keepalive();
    void frame_keepalive();

    // ---- XWayland (xwayland.cpp) ----
#ifdef BC_HAVE_XWAYLAND
    wlr_xwayland* xwayland = nullptr;
    std::unordered_map<wlr_xwayland_surface*, std::unique_ptr<XwaylandRec>> xsurfaces;
#endif
    Listener xwl_ready, xwl_new_surface;
    UnmanagedId next_unmanaged = 1;
    std::vector<XwaylandRec*> unmanaged_order;  // mapped override-redirect, bottom to top

    // ---- taskbars (foreign_toplevel.cpp) ----
    wlr_foreign_toplevel_manager_v1* foreign_manager = nullptr;
    wlr_ext_foreign_toplevel_list_v1* foreign_list = nullptr;

    // ---- touch / tablets (touch_tablet.cpp) ----
    wlr_tablet_manager_v2* tablet_manager = nullptr;
    std::list<std::unique_ptr<TouchRec>> touches;
    std::list<std::unique_ptr<TabletRec>> tablets;
    std::map<wlr_tablet_tool*, std::unique_ptr<TabletToolRec>> tablet_tools;
    std::list<std::unique_ptr<TabletPadRec>> pads;
    wlr_touch* vtouch = nullptr;
    wlr_tablet* vtablet = nullptr;
    wlr_tablet_tool* vtool = nullptr;
    wlr_tablet_pad* vpad = nullptr;
    TabletToolId next_tool = 1;
    TabletPadId next_pad = 1;
    double vtool_x = 0, vtool_y = 0;

    // ---- pointer / keyboard extras (pointer_extras.cpp) ----
    wlr_pointer_constraints_v1* pointer_constraints = nullptr;
    wlr_relative_pointer_manager_v1* relative_pointer = nullptr;
    wlr_virtual_keyboard_manager_v1* virtual_keyboard = nullptr;
    wlr_virtual_pointer_manager_v1* virtual_pointer = nullptr;
    wlr_keyboard_shortcuts_inhibit_manager_v1* shortcuts_inhibit = nullptr;
    std::map<wlr_pointer_constraint_v1*, std::unique_ptr<ConstraintRec>> constraints;
    wlr_pointer_constraint_v1* active_constraint = nullptr;
    std::map<wlr_keyboard_shortcuts_inhibitor_v1*, std::unique_ptr<ShortcutsInhibitorRec>> inhibitors;
    wlr_keyboard_shortcuts_inhibitor_v1* active_inhibitor = nullptr;
    double pointer_origin_x = 0, pointer_origin_y = 0;  // layout origin of the pointer-focused surface
    Listener new_constraint, new_virtual_keyboard, new_virtual_pointer, new_shortcuts_inhibitor;

    // ---- idle (idle.cpp) ----
    wlr_idle_notifier_v1* idle_notifier = nullptr;
    wlr_idle_inhibit_manager_v1* idle_inhibit = nullptr;
    std::map<wlr_idle_inhibitor_v1*, std::unique_ptr<IdleInhibitorRec>> idle_inhibitors;
    bool idle_inhibited = false;
    wl_event_source* idle_check = nullptr;
    Listener new_idle_inhibitor;

    // ---- text input (text_input.cpp) ----
    wlr_text_input_manager_v3* text_input_manager = nullptr;
    wlr_input_method_manager_v2* input_method_manager = nullptr;
    wlr_input_method_v2* input_method = nullptr;
    std::map<wlr_text_input_v3*, std::unique_ptr<TextInputRec>> text_inputs;
    std::map<wlr_input_popup_surface_v2*, std::unique_ptr<InputPopupRec>> input_popups;
    wlr_surface* text_focus = nullptr;
    Listener new_text_input, new_input_method, im_commit, im_new_popup, im_grab_keyboard, im_destroy,
        im_grab_destroy;

    // ---- session lock (session_lock.cpp) ----
    wlr_session_lock_manager_v1* lock_manager = nullptr;
    wlr_session_lock_v1* lock = nullptr;  // the live lock client's lock (null when abandoned)
    LockState lock_state = LockState::Unlocked;
    std::shared_ptr<LockGate> lock_gate = std::make_shared<LockGate>();
    std::map<wlr_session_lock_surface_v1*, std::unique_ptr<LockSurfaceRec>> lock_surfaces;
    std::set<MonitorId> lock_waiting;  // outputs that have not presented since the lock
    wl_event_source* grab_end_idle = nullptr;  // ends a seat grab begun while locked
    Listener new_lock, lock_new_surface, lock_unlock, lock_destroy, pointer_grab_begin, keyboard_grab_begin,
        touch_grab_begin;

    // ---- capture (capture.cpp, screencopy.cpp, image_copy_capture.cpp) ----
    std::map<uint64_t, std::unique_ptr<CaptureJob>> captures;
    uint64_t next_capture = 1;
    std::vector<std::pair<WindowId, std::function<bool()>>> window_commit_waiters;
    wl_global* screencopy_global = nullptr;
    wl_global* image_copy_global = nullptr;
    wl_global* output_source_global = nullptr;
    wl_global* toplevel_source_global = nullptr;

    // ---- gamma (gamma.cpp) ----
    wl_global* gamma_global = nullptr;

    // ---- xdg-toplevel-icon (toplevel_icon.cpp) ----
    wl_global* toplevel_icon_global = nullptr;
    void init_toplevel_icon();
    // set_icon on a toplevel (null: back to its default); applied on its next commit.
    void set_toplevel_icon(wlr_xdg_toplevel* xdg, std::shared_ptr<const IconSet> icon);
    void apply_toplevel_icon(ToplevelRec& t);

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
    // Root of the tree a surface belongs to (toplevel, layer surface, X11
    // window / unmanaged surface, lock surface). xdg / input-method popups
    // resolve to their parent's tree.
    RootRef resolve_root(wlr_surface* surface);
    wlr_surface* surface_by_id(SurfaceId id);
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
    bool place_xdg(ToplevelRec& t, const Rect& frame);
    void set_xdg_visible(ToplevelRec& t, bool visible);
    Point root_origin(ToplevelRec& t);  // layout position of the root surface's origin

    // ---- windows.cpp: either shell ----
    WindowRef window_ref(WindowId id);
    wlr_surface* window_surface(WindowRef w);
    bool place_window(WindowId id, const Rect& frame);
    bool set_window_visible(WindowId id, bool visible);
    bool close_window(WindowId id);
    bool set_window_state(WindowId id, bool maximized, bool fullscreen);
    bool set_window_minimized(WindowId id, bool minimized);
    void set_window_activated(WindowRef w, bool activated);
    // Where a new window of geometry `g` opens: centred in the work area of
    // the output under the cursor (or on its parent), shrunk to fit the work
    // area when larger (never below `min`). Returns the frame to use.
    Rect initial_frame(Size g, WindowId parent, Size min);
    std::optional<SurfaceHit> hit_test_window(WindowId id, double wx, double wy);
    void update_all_window_outputs();
    void push_window_request(WindowId id, WindowRequestKind kind, bool foreign);

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
    void set_front_image(OutputRec& out, uint64_t image_id);  // 0 releases it
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
    void inject_pointer_warp(double x, double y, double dx, double dy);
    void inject_pointer_button(uint32_t button, bool pressed);
    void inject_pointer_axis(uint32_t orientation, double delta, int32_t discrete);
    void publish_cursor_position();
    // The drag icon into the mirror (add_offset: take its commit's dx/dy).
    void publish_drag_icon(bool add_offset);
    void clear_drag_icon();
    void warp(double x, double y);
    // Sends wl_keyboard.modifiers to the focused client when they differ from
    // what it last saw.
    void send_modifiers(const KeyboardModifiers& m);
    KeyboardModifiers sent_modifiers;

    // ---- selection.cpp ----
    void init_selection();

    // ---- xwayland.cpp ----
    bool init_xwayland(std::string* error);
    void shutdown_xwayland();
    XwaylandRec* xwindow(WindowId id);
    XwaylandRec* xrec_of(wlr_surface* surface);  // window or unmanaged surface of this wl_surface
    void publish_xwindow(XwaylandRec& x, uint32_t changes);
    void update_xwindow_outputs(XwaylandRec& x);
    bool place_xwindow(XwaylandRec& x, const Rect& frame);
    void set_xwindow_visible(XwaylandRec& x, bool visible);
    void close_xwindow(XwaylandRec& x);
    void set_xwindow_state(XwaylandRec& x, bool maximized, bool fullscreen);
    void set_xwindow_minimized(XwaylandRec& x, bool minimized);
    void activate_xwindow(XwaylandRec& x, bool activated);
    std::vector<SurfaceNode> xwindow_tree(XwaylandRec& x);
    void publish_unmanaged();
    void update_xwayland_workareas();

    // ---- foreign_toplevel.cpp ----
    void init_foreign_toplevel();
    void foreign_map(std::unique_ptr<ForeignHandles>& h, WindowId id);
    void foreign_update(ForeignHandles* h, const WindowSnapshot& s, bool activated, bool minimized,
                        const std::set<wlr_output*>& outputs);
    void foreign_unmap(std::unique_ptr<ForeignHandles>& h);
    WindowId window_of_ext_handle(wl_resource* handle_resource);

    // ---- touch_tablet.cpp ----
    void init_touch_tablet();
    void add_touch(wlr_touch* touch, bool is_virtual);
    void add_tablet(wlr_tablet* tablet);
    void add_tablet_pad(wlr_tablet_pad* pad);
    void touch_down(SurfaceId surface, int32_t id, double sx, double sy, uint32_t time);
    void touch_motion(int32_t id, double sx, double sy, uint32_t time);
    void touch_up(int32_t id, uint32_t time);
    void touch_cancel();
    TabletToolRec* tool_rec(TabletToolId id);
    void tablet_tool_route(TabletToolId tool, SurfaceId surface, double sx, double sy, const TabletToolAxes& axes);
    void tablet_tool_tip(TabletToolId tool, bool down);
    void tablet_tool_button(TabletToolId tool, uint32_t button, bool pressed);
    void tablet_pad_event(TabletPadId pad, int kind, uint32_t time, uint32_t index, double position, bool on);
    void update_pad_focus(wlr_surface* focus);
    void inject_touch(int kind, int32_t id, double x, double y);
    void inject_tablet(int kind, double x, double y, double pressure, uint32_t button, bool on);
    void shutdown_touch_tablet();

    // ---- pointer_extras.cpp ----
    void init_pointer_extras();
    // Device motion: relative-pointer events and constraint enforcement.
    // Returns false when a lock keeps the cursor where it is.
    bool constrain_motion(double dx, double dy, double udx, double udy, uint32_t time, double* nx, double* ny);
    void update_constraint();         // pointer focus changed
    void update_shortcuts_inhibit();  // keyboard focus changed
    bool shortcuts_inhibited() const { return active_inhibitor != nullptr; }

    // ---- idle.cpp ----
    void init_idle();
    void activity();
    void schedule_idle_check();
    void update_idle_inhibit();
    bool surface_visible(wlr_surface* surface);

    // ---- text_input.cpp ----
    void init_text_input();
    void text_input_focus(wlr_surface* focus);
    // Keys / modifiers routed to the focused client go to the input
    // method's keyboard grab instead when it has one. True when grabbed.
    bool im_grab_key(uint32_t time, uint32_t key, bool pressed);
    bool im_grab_modifiers(const KeyboardModifiers& m);
    // The input method may act (activation, commits) under the lock policy.
    bool input_method_allowed() const;
    wlr_text_input_v3* active_text_input();
    void append_im_popups(WindowId window, std::vector<SurfaceNode>& tree);

    // ---- session_lock.cpp ----
    void init_session_lock();
    bool locked() const { return lock_state != LockState::Unlocked; }
    bool lock_allows(wlr_surface* surface);  // part of a lock surface's tree
    // Whether input synthesized by `pid`'s client may act now (always while
    // unlocked; per LockedVirtualInput while locked).
    bool client_input_allowed(uint32_t pid) const;
    // Records client-synthesized keys/buttons; routing a recorded one while
    // locked (and not allowed) is refused.
    void stamp_client_input(const ClientInputStamp& s);
    bool routed_client_input_refused(uint32_t time_msec, uint32_t code, bool pressed, bool button) const;
    std::deque<ClientInputStamp> client_stamps;
    void lock_output_presented(MonitorId output);
    void refresh_lock_trees();
    void configure_lock_surfaces();
    void focus_lock_surface();
    void end_seat_grabs();
    void set_lock_state(LockState state);

    // ---- capture.cpp ----
    void run_output_capture(std::unique_ptr<CaptureJob> job, OutputImageSlot* source);
    void run_window_capture(std::unique_ptr<CaptureJob> job);
    void capture_done(uint64_t id, bool ok, int render_done_fd);
    void notify_presented(OutputRec& out, OutputImageSlot* image, const std::vector<Rect>& damage, int64_t when_ns);
    void notify_window_commit(WindowId window);
    void fail_output_waiters(OutputRec& out);
    void shutdown_captures();

    // ---- screencopy.cpp / image_copy_capture.cpp / gamma.cpp ----
    void init_screencopy();
    void init_image_copy_capture();
    void init_gamma();
    void apply_gamma(OutputRec& out, wlr_output_state* state);
    void gamma_output_gone(OutputRec& out);
};

}  // namespace brocompositor::wl
