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

    // ---- queries (values) ----
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

    WindowManagerConfig config_;
    std::vector<MonitorSnapshot> monitors_;
    std::map<WorkspaceId, Ws> workspaces_;
    std::map<MonitorId, WorkspaceId> active_;
    std::map<WindowId, Win> windows_;
    std::set<WindowId> moving_;
    WindowId focused_ = kNoWindow;
    WorkspaceId next_ws_ = 1;
    std::vector<Command> out_;
};

}  // namespace brocompositor
