#include "brocompositor/window_manager.h"

#include <algorithm>
#include <cstdlib>
#include <type_traits>

namespace brocompositor {

namespace {

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void erase_id(std::vector<WindowId>& v, WindowId id) {
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
}

}  // namespace

WindowManager::WindowManager(WindowManagerConfig config) : config_(std::move(config)) {}

std::vector<Command> WindowManager::take() {
    std::vector<Command> out;
    out.swap(out_);
    return out;
}

std::vector<Command> WindowManager::handle(const Event& event) {
    std::visit(overloaded{
                   [&](const MonitorsChanged& e) { on_monitors(e); },
                   [&](const WindowAdded& e) { on_added(e.window); },
                   [&](const WindowRemoved& e) { on_removed(e.id); },
                   [&](const WindowChanged& e) { on_changed(e); },
                   [&](const FocusChanged& e) { on_focus(e.id); },
                   [&](const MoveSizeStarted& e) {
                       if (windows_.count(e.id)) moving_.insert(e.id);
                   },
                   [&](const MoveSizeEnded& e) { on_move_size_end(e.id); },
                   [&](const ReservationChanged&) {},
               },
               event);
    return take();
}

// ---------------------------------------------------------------- helpers

bool WindowManager::is_tiled(const Win& w) const {
    if (w.floating || w.snap.minimized || w.snap.maximized || w.snap.fullscreen) return false;
    if (w.sized != WindowState::Normal || w.snapped != SnapZone::None) return false;
    auto it = workspaces_.find(w.ws);
    return it != workspaces_.end() && it->second.layout != LayoutMode::Floating;
}

bool WindowManager::is_active(const Ws& ws) const {
    auto it = active_.find(ws.monitor);
    return it != active_.end() && it->second == ws.id;
}

const MonitorSnapshot* WindowManager::monitor(MonitorId id) const {
    for (const auto& m : monitors_)
        if (m.id == id) return &m;
    return nullptr;
}

MonitorId WindowManager::monitor_at(Point p) const {
    for (const auto& m : monitors_)
        if (m.bounds.contains(p)) return m.id;
    return kNoMonitor;
}

MonitorId WindowManager::fallback_monitor() const {
    for (const auto& m : monitors_)
        if (m.primary) return m.id;
    return monitors_.empty() ? kNoMonitor : monitors_.front().id;
}

void WindowManager::set_shown(WindowId id, Win& w, bool shown) {
    if (w.shown == shown) return;
    // A window the user minimized is already off screen; hiding it would only
    // fight the user. It stays "shown" and comes back when they restore it.
    if (!shown && w.snap.minimized) return;
    w.shown = shown;
    if (!shown) w.placed.reset();
    emit(SetWindowVisible{id, shown});
}

void WindowManager::touch_focus(Ws& ws, WindowId id) {
    erase_id(ws.focus_order, id);
    ws.focus_order.insert(ws.focus_order.begin(), id);
}

void WindowManager::detach(WindowId id, Win& w) {
    auto it = workspaces_.find(w.ws);
    if (it != workspaces_.end()) {
        erase_id(it->second.order, id);
        erase_id(it->second.focus_order, id);
    }
    w.ws = kNoWorkspace;
}

void WindowManager::attach(WindowId id, Win& w, WorkspaceId ws_id) {
    auto it = workspaces_.find(ws_id);
    if (it == workspaces_.end()) return;
    w.ws = ws_id;
    it->second.order.push_back(id);
    it->second.focus_order.push_back(id);
    set_shown(id, w, is_active(it->second));
}

void WindowManager::do_relayout(WorkspaceId id) {
    auto it = workspaces_.find(id);
    if (it == workspaces_.end() || !is_active(it->second)) return;
    const Ws& ws = it->second;
    const MonitorSnapshot* mon = monitor(ws.monitor);
    if (!mon) return;
    std::vector<WindowId> tiled;
    for (WindowId wid : ws.order) {
        const Win& w = windows_.at(wid);
        if (is_tiled(w) && w.shown) tiled.push_back(wid);
    }
    LayoutResult r = compute_layout(ws.layout, mon->work_area, tiled, config_.layout);
    for (const Placement& p : r.placements) {
        if (moving_.count(p.id)) continue;
        Win& w = windows_.at(p.id);
        // The slot holds the whole frame, the host's decoration included.
        const Rect client = client_in(w, p.rect, false);
        if (w.placed == client && w.snap.frame == client) continue;
        w.placed = client;
        emit(PlaceWindow{p.id, client});
    }
}

void WindowManager::do_activate(WorkspaceId id, bool refocus) {
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) return;
    Ws& ws = it->second;
    WorkspaceId old = active_.count(ws.monitor) ? active_[ws.monitor] : kNoWorkspace;
    if (old == id) return;
    active_[ws.monitor] = id;
    // Show the new workspace first, then hide the old one, so the monitor is
    // never momentarily empty.
    for (WindowId wid : ws.order) set_shown(wid, windows_.at(wid), true);
    do_relayout(id);  // floating windows: the backend restores their pre-hide position
    bool hid_focused = false;
    auto old_it = workspaces_.find(old);
    if (old_it != workspaces_.end()) {
        for (WindowId wid : old_it->second.order) {
            set_shown(wid, windows_.at(wid), false);
            hid_focused |= wid == focused_;
        }
    }
    if (!refocus) return;
    auto f = windows_.find(focused_);
    if (f != windows_.end() && f->second.ws == id) return;
    for (WindowId wid : ws.focus_order) {
        if (!windows_.at(wid).snap.minimized) {
            emit(FocusWindow{wid});
            return;
        }
    }
    // Nothing to focus here, but the focused window was just hidden: hidden
    // windows must not keep keyboard focus, so ask for focus to go "nowhere"
    // (the backend picks a neutral target, e.g. the desktop or the host).
    if (hid_focused) {
        focused_ = kNoWindow;
        emit(FocusWindow{kNoWindow});
    }
}

// ---------------------------------------------------------------- events

void WindowManager::on_monitors(const MonitorsChanged& e) {
    std::vector<MonitorId> removed;
    for (const auto& m : monitors_) {
        bool still = std::any_of(e.monitors.begin(), e.monitors.end(),
                                 [&](const MonitorSnapshot& n) { return n.id == m.id; });
        if (!still) removed.push_back(m.id);
    }
    std::vector<MonitorId> added;
    for (const auto& n : e.monitors)
        if (!monitor(n.id)) added.push_back(n.id);
    reported_ = e.monitors;
    apply_reservations();

    for (MonitorId mid : added) {
        bool has_ws = false;
        for (auto& [wid, ws] : workspaces_) has_ws |= ws.monitor == mid;
        if (has_ws) continue;
        uint32_t n = std::max<uint32_t>(1, config_.workspaces_per_monitor);
        for (uint32_t i = 0; i < n; ++i) add_workspace(mid, std::string());
    }

    MonitorId target = fallback_monitor();
    for (MonitorId mid : removed) {
        active_.erase(mid);
        if (target == kNoMonitor) continue;
        for (auto& [wid, ws] : workspaces_) {
            if (ws.monitor != mid) continue;
            ws.monitor = target;
            if (!active_.count(target)) active_[target] = wid;
            // Workspaces that arrive on another monitor come up hidden.
            if (!is_active(ws))
                for (WindowId w : ws.order) set_shown(w, windows_.at(w), false);
        }
    }

    // Windows that arrived before any monitor existed.
    for (auto& [id, w] : windows_) {
        if (w.ws != kNoWorkspace) continue;
        MonitorId mid = monitor(w.snap.monitor) ? w.snap.monitor : fallback_monitor();
        if (active_.count(mid)) attach(id, w, active_[mid]);
    }

    for (auto& [mid, wsid] : active_) do_relayout(wsid);
    refit_sized();
}

void WindowManager::on_added(const WindowSnapshot& s) {
    if (s.id == kNoWindow || windows_.count(s.id)) return;
    if (config_.manage && !config_.manage(s)) return;
    Win w;
    w.snap = s;
    w.floating = config_.float_rule ? config_.float_rule(s) : (s.owner != kNoWindow || !s.resizable);
    w.floating_rect = s.frame;
    w.shown = true;

    WorkspaceId target = kNoWorkspace;
    auto owner = windows_.find(s.owner);
    if (owner != windows_.end() && owner->second.ws != kNoWorkspace) {
        target = owner->second.ws;
    } else {
        MonitorId mid = monitor(s.monitor) ? s.monitor : fallback_monitor();
        if (active_.count(mid)) target = active_[mid];
    }
    auto [it, ok] = windows_.emplace(s.id, std::move(w));
    stack_add(s.id);
    if (target != kNoWorkspace) {
        attach(s.id, it->second, target);
        // A decorated window placed with its client at the top of the work
        // area would have its title bar under the shell's panel (or off
        // screen): move it down by the frame's top band.
        Win& nw = it->second;
        const Margins in = insets_now(nw);
        if (in.top > 0 && !is_tiled(nw) && !nw.snap.maximized && !nw.snap.fullscreen) {
            if (const MonitorSnapshot* m = window_monitor(nw)) {
                const Rect& wa = m->work_area;
                if (!wa.empty() && nw.snap.frame.y - in.top < wa.y) {
                    Rect f = nw.snap.frame;
                    f.y = wa.y + in.top;
                    nw.floating_rect = f;
                    nw.placed = f;
                    emit(PlaceWindow{s.id, f});
                }
            }
        }
        do_relayout(target);
    }
    if (config_.focus_on_map) {
        stack_raise(s.id);
        emit(FocusWindow{s.id});
    }
}

void WindowManager::on_removed(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return;
    WorkspaceId ws_id = it->second.ws;
    detach(id, it->second);
    windows_.erase(it);
    moving_.erase(id);
    stack_remove(id);
    if (drag_ && drag_->info.window == id) drag_.reset();
    bool was_focused = focused_ == id;
    if (was_focused) focused_ = kNoWindow;
    auto ws = workspaces_.find(ws_id);
    if (ws == workspaces_.end()) return;
    do_relayout(ws_id);
    if (was_focused && config_.refocus_on_close && is_active(ws->second)) {
        for (WindowId wid : ws->second.focus_order) {
            if (!windows_.at(wid).snap.minimized) {
                emit(FocusWindow{wid});
                break;
            }
        }
    }
}

void WindowManager::on_changed(const WindowChanged& e) {
    auto it = windows_.find(e.window.id);
    if (it == windows_.end()) return;
    Win& w = it->second;
    WindowSnapshot old = w.snap;
    w.snap = e.window;
    bool state = old.minimized != w.snap.minimized || old.maximized != w.snap.maximized ||
                 old.fullscreen != w.snap.fullscreen;
    if (state && !w.snap.minimized && !w.snap.maximized) w.placed.reset();
    // Left maximized / fullscreen by itself (the client, the user, a drag):
    // the core no longer keeps it fitted or owes it a restore. (Snapping a
    // maximized window leaves the state too, but keeps its restore frame.)
    if ((old.maximized || old.fullscreen) && !w.snap.maximized && !w.snap.fullscreen &&
        w.snapped == SnapZone::None) {
        w.sized = WindowState::Normal;
        w.restore_rect.reset();
    }
    bool moving = moving_.count(w.snap.id) != 0;

    if (w.floating && w.shown && !moving && !w.snap.minimized && !w.snap.maximized &&
        !w.snap.fullscreen && w.sized == WindowState::Normal && w.snapped == SnapZone::None)
        w.floating_rect = w.snap.frame;

    // The host's frame came or went: whatever the core keeps fitted is
    // re-fitted around the new decoration.
    if (old.decorated != w.snap.decorated) {
        w.placed.reset();
        refit_sized();
        state = true;
    }

    // Moved to another monitor without a drag (keyboard snap, app-initiated):
    // follow it to that monitor's active workspace.
    auto ws = workspaces_.find(w.ws);
    if (!moving && w.shown && ws != workspaces_.end() && monitor(w.snap.monitor) &&
        w.snap.monitor != ws->second.monitor && active_.count(w.snap.monitor)) {
        WorkspaceId from = w.ws;
        detach(w.snap.id, w);
        attach(w.snap.id, w, active_[w.snap.monitor]);
        do_relayout(from);
        do_relayout(w.ws);
        return;
    }
    if (state) do_relayout(w.ws);
}

void WindowManager::on_focus(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end()) {
        focused_ = kNoWindow;
        return;
    }
    focused_ = id;
    if (config_.raise_on_focus) stack_raise(id);
    auto ws = workspaces_.find(it->second.ws);
    if (ws == workspaces_.end()) return;
    touch_focus(ws->second, id);
    if (config_.focus_switches_workspace && !is_active(ws->second)) do_activate(ws->first, false);
}

void WindowManager::on_move_size_end(WindowId id) {
    moving_.erase(id);
    auto it = windows_.find(id);
    if (it == windows_.end()) return;
    Win& w = it->second;
    auto ws_it = workspaces_.find(w.ws);
    if (ws_it == workspaces_.end()) return;
    Point c = w.snap.frame.center();
    MonitorId dropped = monitor_at(c);
    if (dropped != kNoMonitor && dropped != ws_it->second.monitor && active_.count(dropped)) {
        WorkspaceId from = w.ws;
        detach(id, w);
        attach(id, w, active_[dropped]);
        if (w.floating) w.floating_rect = w.snap.frame;
        do_relayout(from);
        do_relayout(w.ws);
        return;
    }
    if (w.floating) {
        w.floating_rect = w.snap.frame;
        return;
    }
    if (!is_tiled(w)) return;
    // Dropped over another tiled window: swap their slots.
    Ws& ws = ws_it->second;
    for (WindowId other : ws.order) {
        if (other == id) continue;
        const Win& o = windows_.at(other);
        if (is_tiled(o) && o.placed && o.placed->contains(c)) {
            auto a = std::find(ws.order.begin(), ws.order.end(), id);
            auto b = std::find(ws.order.begin(), ws.order.end(), other);
            std::iter_swap(a, b);
            break;
        }
    }
    w.placed.reset();  // re-place even if the slot is unchanged (snap back)
    do_relayout(ws.id);
}

// ---------------------------------------------------------------- actions

WorkspaceId WindowManager::add_workspace(MonitorId monitor_id, std::string name) {
    Ws ws;
    ws.id = next_ws_++;
    ws.name = name.empty() ? std::to_string(ws.id) : std::move(name);
    ws.monitor = monitor_id;
    ws.layout = config_.default_layout;
    WorkspaceId id = ws.id;
    workspaces_.emplace(id, std::move(ws));
    if (!active_.count(monitor_id)) active_[monitor_id] = id;
    return id;
}

std::vector<Command> WindowManager::remove_workspace(WorkspaceId id) {
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) return {};
    MonitorId mid = it->second.monitor;
    WorkspaceId heir = kNoWorkspace;
    for (auto& [wid, ws] : workspaces_)
        if (wid != id && ws.monitor == mid && (heir == kNoWorkspace || is_active(ws))) heir = wid;
    if (heir == kNoWorkspace) return {};  // never leave a monitor without a workspace
    if (is_active(it->second)) do_activate(heir, true);
    std::vector<WindowId> moved = it->second.order;
    for (WindowId wid : moved) {
        Win& w = windows_.at(wid);
        detach(wid, w);
        attach(wid, w, heir);
    }
    workspaces_.erase(id);
    do_relayout(heir);
    return take();
}

std::vector<Command> WindowManager::activate_workspace(WorkspaceId id) {
    do_activate(id, true);
    return take();
}

std::vector<Command> WindowManager::move_window_to_workspace(WindowId id, WorkspaceId target,
                                                             bool follow) {
    auto it = windows_.find(id);
    auto tgt = workspaces_.find(target);
    if (it == windows_.end() || tgt == workspaces_.end()) return {};
    Win& w = it->second;
    WorkspaceId from = w.ws;
    if (from != target) {
        auto src = workspaces_.find(from);
        // A floating window keeps its offset within the work area when it
        // changes monitor.
        if (w.floating && src != workspaces_.end() && src->second.monitor != tgt->second.monitor) {
            const MonitorSnapshot* a = monitor(src->second.monitor);
            const MonitorSnapshot* b = monitor(tgt->second.monitor);
            if (a && b) {
                w.floating_rect.x += b->work_area.x - a->work_area.x;
                w.floating_rect.y += b->work_area.y - a->work_area.y;
                if (is_active(tgt->second)) emit(PlaceWindow{id, w.floating_rect});
            }
        }
        detach(id, w);
        attach(id, w, target);
        do_relayout(from);
        do_relayout(target);
    }
    if (follow) {
        do_activate(target, false);
        emit(FocusWindow{id});
    }
    return take();
}

std::vector<Command> WindowManager::set_layout(WorkspaceId id, LayoutMode mode) {
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) return {};
    it->second.layout = mode;
    if (mode == LayoutMode::Floating) {
        // Back to floating: return each window to the geometry it had.
        for (WindowId wid : it->second.order) {
            Win& w = windows_.at(wid);
            if (w.placed && w.shown && !w.floating_rect.empty()) emit(PlaceWindow{wid, w.floating_rect});
            w.placed.reset();
        }
    }
    do_relayout(id);
    return take();
}

std::vector<Command> WindowManager::set_layout_config(const LayoutConfig& config) {
    config_.layout = config;
    for (auto& [mid, wsid] : active_) do_relayout(wsid);
    return take();
}

std::vector<Command> WindowManager::set_floating(WindowId id, bool floating) {
    auto it = windows_.find(id);
    if (it == windows_.end() || it->second.floating == floating) return {};
    Win& w = it->second;
    w.floating = floating;
    if (floating) {
        w.placed.reset();
        if (!w.floating_rect.empty() && w.shown) emit(PlaceWindow{id, w.floating_rect});
    }
    do_relayout(w.ws);
    return take();
}

std::vector<Command> WindowManager::focus(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    auto ws = workspaces_.find(it->second.ws);
    if (ws != workspaces_.end() && !is_active(ws->second)) do_activate(ws->first, false);
    // Raised now rather than when the backend reports the focus, so the host
    // draws it on top in the same frame.
    if (config_.raise_on_focus) stack_raise(id);
    emit(FocusWindow{id});
    return take();
}

std::vector<Command> WindowManager::focus_direction(Direction dir) {
    auto cur = windows_.find(focused_);
    if (cur == windows_.end()) return {};
    Point c = cur->second.snap.frame.center();
    WindowId best = kNoWindow;
    int64_t best_score = 0;
    for (const auto& [id, w] : windows_) {
        if (id == focused_ || !w.shown || w.snap.minimized) continue;
        auto ws = workspaces_.find(w.ws);
        if (ws == workspaces_.end() || !is_active(ws->second)) continue;
        Point p = w.snap.frame.center();
        int64_t dx = int64_t(p.x) - c.x, dy = int64_t(p.y) - c.y;
        int64_t along = 0, across = 0;
        switch (dir) {
            case Direction::Left: along = -dx; across = dy; break;
            case Direction::Right: along = dx; across = dy; break;
            case Direction::Up: along = -dy; across = dx; break;
            case Direction::Down: along = dy; across = dx; break;
        }
        if (along <= 0) continue;
        int64_t score = along + 2 * (across < 0 ? -across : across);
        if (best == kNoWindow || score < best_score) {
            best = id;
            best_score = score;
        }
    }
    if (best != kNoWindow) emit(FocusWindow{best});
    return take();
}

std::vector<Command> WindowManager::swap(WindowId a, WindowId b) {
    auto ia = windows_.find(a), ib = windows_.find(b);
    if (ia == windows_.end() || ib == windows_.end() || ia->second.ws != ib->second.ws) return {};
    Ws& ws = workspaces_.at(ia->second.ws);
    std::iter_swap(std::find(ws.order.begin(), ws.order.end(), a),
                   std::find(ws.order.begin(), ws.order.end(), b));
    do_relayout(ws.id);
    return take();
}

std::vector<Command> WindowManager::relayout(WorkspaceId id) {
    auto it = workspaces_.find(id);
    if (it != workspaces_.end())
        for (WindowId wid : it->second.order) windows_.at(wid).placed.reset();
    do_relayout(id);
    return take();
}

// ---------------------------------------------------------------- queries

WorkspaceView WindowManager::view(const Ws& ws) const {
    WorkspaceView v;
    v.id = ws.id;
    v.name = ws.name;
    v.monitor = ws.monitor;
    v.layout = ws.layout;
    v.active = is_active(ws);
    v.windows = ws.order;
    v.focus_order = ws.focus_order;
    return v;
}

std::vector<WorkspaceView> WindowManager::workspaces() const {
    std::vector<WorkspaceView> out;
    for (const auto& [id, ws] : workspaces_) out.push_back(view(ws));
    return out;
}

std::optional<WorkspaceView> WindowManager::workspace(WorkspaceId id) const {
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) return std::nullopt;
    return view(it->second);
}

WorkspaceId WindowManager::active_workspace(MonitorId monitor_id) const {
    auto it = active_.find(monitor_id);
    return it == active_.end() ? kNoWorkspace : it->second;
}

std::optional<WindowView> WindowManager::window(WindowId id) const {
    auto it = windows_.find(id);
    if (it == windows_.end()) return std::nullopt;
    WindowView v;
    v.snapshot = it->second.snap;
    v.workspace = it->second.ws;
    v.floating = it->second.floating;
    v.shown = it->second.shown;
    v.tiled = is_tiled(it->second);
    v.snap = it->second.snap.maximized ? SnapZone::Maximize : it->second.snapped;
    v.decoration = insets_now(it->second);
    v.framed = framed_now(it->second);
    const Rect& f = it->second.snap.frame;
    v.outer = Rect{f.x - v.decoration.left, f.y - v.decoration.top, f.width + v.decoration.horizontal(),
                   f.height + v.decoration.vertical()};
    return v;
}

std::vector<WindowId> WindowManager::windows() const {
    std::vector<WindowId> out;
    for (const auto& [id, w] : windows_) out.push_back(id);
    return out;
}

}  // namespace brocompositor
