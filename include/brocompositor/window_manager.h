// The portable window-management core: monitors, workspaces, focus history,
// tiling policy. It is a pure state machine with no threads and no OS calls.
//
//   facts in:      handle(const Event&)        -- from a backend's EventQueue
//   decisions out: std::vector<Command>         -- for the same backend to execute
//
// Host actions (switch workspace, move a window, change layout, directional
// focus) are methods that also return commands. The core never assumes a
// command succeeded: it records what it asked for and keeps reconciling
// against the facts that come back.
//
// Safety default: every workspace starts in LayoutMode::Floating, in which
// the core never moves a window. Nothing is retiled until the host opts a
// workspace into a tiling layout.
//
// Window-management policy a shell may tune also lives here: window states
// (minimize / maximize / fullscreen / restore), edge reservations for the
// shell's own panels (they shrink the work area maximize and tiling use), and
// how a pointer press on a window frame is interpreted (InteractionConfig,
// classify_press) for hosts that draw the desktop themselves.
#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/events.h"
#include "brocompositor/layout.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace brocompositor {

// Modifier bits for the pointer-interaction policy.
namespace modifier {
inline constexpr uint32_t Shift = 1u << 0;
inline constexpr uint32_t Ctrl = 1u << 1;
inline constexpr uint32_t Alt = 1u << 2;
inline constexpr uint32_t Super = 1u << 3;
}  // namespace modifier

// Resize edges (the xdg_toplevel resize_edge bit values).
namespace resize_edge {
inline constexpr uint32_t Top = 1;
inline constexpr uint32_t Bottom = 2;
inline constexpr uint32_t Left = 4;
inline constexpr uint32_t Right = 8;
}  // namespace resize_edge

enum class PressButton : uint32_t { Left = 0, Right = 1, Middle = 2, Other = 3 };

// How a press on a window is read by a host that routes the pointer itself
// (bro's DRM shell host). Coordinates are those of WindowSnapshot::frame.
struct InteractionConfig {
    int32_t titlebar_height = 38;  // band at the top of a frame whose left-press drags the window (0: off)
    int32_t resize_border = 6;     // band inside the frame edges whose left-press resizes (0: off)
    // Holding any of these: left-press anywhere moves, right-press resizes
    // from the bottom-right corner (0: off).
    uint32_t drag_modifiers = modifier::Super | modifier::Alt;
};

enum class PressAction : uint32_t { None = 0, Move = 1, Resize = 2 };

struct PressDecision {
    PressAction action = PressAction::None;
    uint32_t edges = 0;      // resize_edge bits for Resize
    bool immediate = true;   // false: start only once the pointer has moved a few pixels
    bool forward = false;    // the press also goes to the client (a title-bar click stays a click)
    bool operator==(const PressDecision&) const = default;
};

// An edge band the shell keeps for its own panels. Shell reservation ids
// start at kShellReservationBase, apart from the ids a platform mints for
// its own reservations (layer-shell exclusive zones, appbars), which reach
// the host as ReservationChanged events.
inline constexpr ReservationId kShellReservationBase = ReservationId(1) << 48;

struct EdgeReservation {
    ReservationId id = kNoReservation;
    MonitorId monitor = kNoMonitor;  // as requested; kNoMonitor follows the primary monitor
    Edge edge = Edge::Top;
    int32_t thickness = 0;
    Rect rect;                       // the granted band (empty while its monitor is absent)
};

struct WindowManagerConfig {
    LayoutConfig layout;
    LayoutMode default_layout = LayoutMode::Floating;
    uint32_t workspaces_per_monitor = 1;  // created when a monitor first appears
    bool focus_switches_workspace = true;  // a hidden window gaining focus brings its workspace up
    bool refocus_on_close = true;          // focus the MRU window when the focused one closes
    // Which reported windows the core manages at all (null: all of them).
    std::function<bool(const WindowSnapshot&)> manage;
    // Which managed windows start floating (null: transient or fixed-size ones).
    std::function<bool(const WindowSnapshot&)> float_rule;
    InteractionConfig interaction;
};

struct WorkspaceView {
    WorkspaceId id = kNoWorkspace;
    std::string name;
    MonitorId monitor = kNoMonitor;
    LayoutMode layout = LayoutMode::Floating;
    bool active = false;                // shown on its monitor
    std::vector<WindowId> windows;      // layout order (tiled and floating)
    std::vector<WindowId> focus_order;  // most recently focused first
};

struct WindowView {
    WindowSnapshot snapshot;  // last facts reported by the backend
    WorkspaceId workspace = kNoWorkspace;
    bool floating = false;
    bool shown = true;        // false while hidden by a workspace switch
    bool tiled = false;       // currently participates in its workspace's layout
};

class WindowManager {
public:
    explicit WindowManager(WindowManagerConfig config = WindowManagerConfig());

    std::vector<Command> handle(const Event& event);

    // ---- host actions ----
    WorkspaceId add_workspace(MonitorId monitor, std::string name);
    std::vector<Command> remove_workspace(WorkspaceId id);
    std::vector<Command> activate_workspace(WorkspaceId id);
    std::vector<Command> move_window_to_workspace(WindowId id, WorkspaceId target, bool follow);
    std::vector<Command> set_layout(WorkspaceId id, LayoutMode mode);
    std::vector<Command> set_layout_config(const LayoutConfig& config);
    std::vector<Command> set_floating(WindowId id, bool floating);
    std::vector<Command> focus(WindowId id);
    std::vector<Command> focus_direction(Direction dir);
    std::vector<Command> swap(WindowId a, WindowId b);
    std::vector<Command> relayout(WorkspaceId id);

    // ---- window states ----
    // Each returns the commands to send (SetWindowState, then the PlaceWindow
    // giving the state its geometry, plus any focus change); empty when the
    // window is unknown or already in that state. Maximize fills the work
    // area of the window's workspace monitor (shell reservations included);
    // the frame it had is remembered and restore() puts it back (a tiled
    // window is re-tiled instead). Restoring a minimized window brings back
    // the state it was minimized from and focuses it. A window the core
    // maximized follows its work area when reservations or monitors change.
    std::vector<Command> minimize(WindowId id);
    std::vector<Command> maximize(WindowId id);
    std::vector<Command> fullscreen(WindowId id);
    std::vector<Command> restore(WindowId id);

    // ---- edge reservations (the shell's own panels) ----
    // reserve_edge carves `thickness` pixels off `edge` of the monitor's work
    // area (kNoMonitor: the primary monitor, following it when it changes),
    // after the platform's own reservations and earlier shell reservations.
    // Tiled workspaces and windows the core maximized are re-placed.
    struct ReserveResult {
        ReservationId id = kNoReservation;  // kNoReservation: thickness <= 0
        std::vector<Command> commands;
    };
    ReserveResult reserve_edge(MonitorId monitor, Edge edge, int32_t thickness);
    std::vector<Command> release_edge(ReservationId id);
    std::optional<EdgeReservation> reservation(ReservationId id) const;
    std::vector<EdgeReservation> reservations() const;
    // Bounds minus platform and shell reservations (empty for an unknown monitor).
    Rect work_area(MonitorId monitor) const;

    // ---- pointer interaction policy ----
    const InteractionConfig& interaction() const { return config_.interaction; }
    void set_interaction(const InteractionConfig& config) { config_.interaction = config; }
    // What a press at `p` (frame coordinates) with `modifiers` held means for
    // window `id` under the interaction policy; None for an unknown window or
    // a press the client should simply get.
    PressDecision classify_press(WindowId id, Point p, uint32_t modifiers, PressButton button) const;

    // ---- queries (values) ----
    // Monitors as reported, with work_area also excluding shell reservations.
    const std::vector<MonitorSnapshot>& monitors() const { return monitors_; }
    std::vector<WorkspaceView> workspaces() const;
    std::optional<WorkspaceView> workspace(WorkspaceId id) const;
    WorkspaceId active_workspace(MonitorId monitor) const;
    std::optional<WindowView> window(WindowId id) const;
    std::vector<WindowId> windows() const;
    WindowId focused() const { return focused_; }

private:
    struct Win {
        WindowSnapshot snap;
        WorkspaceId ws = kNoWorkspace;
        bool floating = false;
        bool shown = true;
        Rect floating_rect;
        std::optional<Rect> placed;  // last frame the core asked for
        std::optional<Rect> restore_rect;  // frame before the core maximized / fullscreened it
        WindowState sized = WindowState::Normal;  // Maximized / Fullscreen: the core put it there and keeps it fitted
    };
    struct Ws {
        WorkspaceId id = kNoWorkspace;
        std::string name;
        MonitorId monitor = kNoMonitor;
        LayoutMode layout = LayoutMode::Floating;
        std::vector<WindowId> order;
        std::vector<WindowId> focus_order;
    };

    // event handlers
    void on_monitors(const MonitorsChanged& e);
    void on_added(const WindowSnapshot& s);
    void on_removed(WindowId id);
    void on_changed(const WindowChanged& e);
    void on_focus(WindowId id);
    void on_move_size_end(WindowId id);

    // helpers (all append to out_)
    void emit(Command c) { out_.push_back(std::move(c)); }
    std::vector<Command> take();
    bool is_tiled(const Win& w) const;
    bool is_active(const Ws& ws) const;
    const MonitorSnapshot* monitor(MonitorId id) const;
    MonitorId monitor_at(Point p) const;
    MonitorId fallback_monitor() const;
    void do_relayout(WorkspaceId id);
    void do_activate(WorkspaceId id, bool refocus);
    void set_shown(WindowId id, Win& w, bool shown);
    void detach(WindowId id, Win& w);
    void attach(WindowId id, Win& w, WorkspaceId ws);
    void touch_focus(Ws& ws, WindowId id);
    WorkspaceView view(const Ws& ws) const;
    void apply_reservations();  // monitors_ = reported_ minus shell reservations
    void refit_sized();         // re-place windows the core maximized / fullscreened
    const MonitorSnapshot* window_monitor(const Win& w) const;
    void refocus_away_from(WindowId id);
    std::vector<Command> enter_sized_state(WindowId id, WindowState state);

    WindowManagerConfig config_;
    std::vector<MonitorSnapshot> reported_;  // as the backend reported them
    std::vector<MonitorSnapshot> monitors_;  // reported_ with shell reservations applied
    std::map<ReservationId, EdgeReservation> reservations_;
    ReservationId next_reservation_ = kShellReservationBase + 1;
    std::map<WorkspaceId, Ws> workspaces_;
    std::map<MonitorId, WorkspaceId> active_;
    std::map<WindowId, Win> windows_;
    std::set<WindowId> moving_;
    WindowId focused_ = kNoWindow;
    WorkspaceId next_ws_ = 1;
    std::vector<Command> out_;
};

}  // namespace brocompositor
