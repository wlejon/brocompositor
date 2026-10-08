// WindowManager policy a shell tunes: window states, the shell's edge
// reservations and the pointer-interaction rules (window_manager.h).
#include "brocompositor/window_manager.h"

#include <algorithm>

namespace brocompositor {

// ---------------------------------------------------------------- helpers

const MonitorSnapshot* WindowManager::window_monitor(const Win& w) const {
    auto ws = workspaces_.find(w.ws);
    if (ws != workspaces_.end())
        if (const MonitorSnapshot* m = monitor(ws->second.monitor)) return m;
    if (const MonitorSnapshot* m = monitor(w.snap.monitor)) return m;
    return monitor(fallback_monitor());
}

void WindowManager::refocus_away_from(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return;
    auto ws = workspaces_.find(it->second.ws);
    if (ws != workspaces_.end()) {
        for (WindowId wid : ws->second.focus_order) {
            if (wid == id) continue;
            const Win& o = windows_.at(wid);
            if (o.shown && !o.snap.minimized) {
                emit(FocusWindow{wid});
                return;
            }
        }
    }
    focused_ = kNoWindow;
    emit(FocusWindow{kNoWindow});
}

void WindowManager::apply_reservations() {
    monitors_ = reported_;
    for (auto& [id, r] : reservations_) {
        r.rect = Rect{};
        MonitorId mid = r.monitor == kNoMonitor ? fallback_monitor() : r.monitor;
        MonitorSnapshot* m = nullptr;
        for (auto& x : monitors_)
            if (x.id == mid) m = &x;
        if (!m || m->work_area.empty()) continue;
        Rect& wa = m->work_area;
        switch (r.edge) {
            case Edge::Top: {
                int32_t t = std::min(r.thickness, wa.height);
                r.rect = Rect{wa.x, wa.y, wa.width, t};
                wa.y += t;
                wa.height -= t;
                break;
            }
            case Edge::Bottom: {
                int32_t t = std::min(r.thickness, wa.height);
                r.rect = Rect{wa.x, wa.bottom() - t, wa.width, t};
                wa.height -= t;
                break;
            }
            case Edge::Left: {
                int32_t t = std::min(r.thickness, wa.width);
                r.rect = Rect{wa.x, wa.y, t, wa.height};
                wa.x += t;
                wa.width -= t;
                break;
            }
            case Edge::Right: {
                int32_t t = std::min(r.thickness, wa.width);
                r.rect = Rect{wa.right() - t, wa.y, t, wa.height};
                wa.width -= t;
                break;
            }
        }
    }
}

void WindowManager::refit_sized() {
    for (auto& [id, w] : windows_) {
        if (w.snap.minimized) continue;
        if (w.sized == WindowState::Normal) {
            if (w.snapped != SnapZone::None) {
                auto ws = workspaces_.find(w.ws);
                if (ws != workspaces_.end() && is_active(ws->second)) place_snapped(id, w);
            }
            continue;
        }
        auto ws = workspaces_.find(w.ws);
        if (ws == workspaces_.end() || !is_active(ws->second)) continue;
        const MonitorSnapshot* m = window_monitor(w);
        if (!m) continue;
        Rect target = w.sized == WindowState::Maximized ? client_in(w, m->work_area, true) : m->bounds;
        if (target.empty() || w.placed == target) continue;
        w.placed = target;
        emit(PlaceWindow{id, target});
    }
}

// ---------------------------------------------------------------- states

std::vector<Command> WindowManager::enter_sized_state(WindowId id, WindowState state) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    Win& w = it->second;
    const bool max = state == WindowState::Maximized;
    const bool already = max ? (w.snap.maximized && !w.snap.fullscreen) : w.snap.fullscreen;
    if (already && !w.snap.minimized) return {};
    const MonitorSnapshot* m = window_monitor(w);
    if (!m) return {};
    Rect target = max ? client_in(w, m->work_area, true) : m->bounds;
    if (target.empty()) return {};

    const bool was_minimized = w.snap.minimized;
    if (was_minimized) {
        auto ws = workspaces_.find(w.ws);
        if (ws != workspaces_.end() && !is_active(ws->second)) do_activate(ws->first, false);
    }
    if (!w.restore_rect && !w.snap.maximized && !w.snap.fullscreen && !w.snap.frame.empty())
        w.restore_rect = w.snap.frame;
    w.sized = state;
    w.snapped = SnapZone::None;
    emit(SetWindowState{id, state});
    w.placed = target;
    emit(PlaceWindow{id, target});
    do_relayout(w.ws);  // the rest of a tiled workspace closes the gap
    if (was_minimized) emit(FocusWindow{id});
    return take();
}

std::vector<Command> WindowManager::maximize(WindowId id) {
    return enter_sized_state(id, WindowState::Maximized);
}

std::vector<Command> WindowManager::fullscreen(WindowId id) {
    return enter_sized_state(id, WindowState::Fullscreen);
}

std::vector<Command> WindowManager::minimize(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end() || it->second.snap.minimized) return {};
    emit(SetWindowState{id, WindowState::Minimized});
    if (focused_ == id) refocus_away_from(id);
    return take();
}

std::vector<Command> WindowManager::restore(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    Win& w = it->second;
    auto ws = workspaces_.find(w.ws);

    if (w.snap.minimized) {
        // Back to the state it was minimized from.
        if (ws != workspaces_.end() && !is_active(ws->second)) do_activate(ws->first, false);
        WindowState back = w.snap.fullscreen ? WindowState::Fullscreen
                           : w.snap.maximized ? WindowState::Maximized
                                              : WindowState::Normal;
        emit(SetWindowState{id, back});
        emit(FocusWindow{id});
        return take();
    }
    if (!w.snap.maximized && !w.snap.fullscreen && w.sized == WindowState::Normal) {
        if (w.snapped == SnapZone::None) return {};
        // Snapped: back to the frame it had before (the state never changed).
        w.snapped = SnapZone::None;
        std::optional<Rect> back = w.restore_rect;
        w.restore_rect.reset();
        w.placed.reset();
        const bool tiles = !w.floating && ws != workspaces_.end() && ws->second.layout != LayoutMode::Floating;
        if (tiles) {
            do_relayout(w.ws);
        } else if (back && !back->empty()) {
            emit(PlaceWindow{id, *back});
            if (w.floating) w.floating_rect = *back;
        }
        return take();
    }

    emit(SetWindowState{id, WindowState::Normal});
    std::optional<Rect> back = w.restore_rect;
    if (!back && !w.floating_rect.empty()) back = w.floating_rect;
    w.sized = WindowState::Normal;
    w.snapped = SnapZone::None;
    w.restore_rect.reset();
    w.placed.reset();
    const bool tiles = !w.floating && ws != workspaces_.end() && ws->second.layout != LayoutMode::Floating;
    if (!tiles && back && !back->empty()) {
        // A tiled window is re-tiled once the backend reports it normal.
        emit(PlaceWindow{id, *back});
        if (w.floating) w.floating_rect = *back;
    }
    return take();
}

// ---------------------------------------------------------------- reservations

WindowManager::ReserveResult WindowManager::reserve_edge(MonitorId monitor_id, Edge edge,
                                                         int32_t thickness) {
    if (thickness <= 0) return {};
    EdgeReservation r;
    r.id = next_reservation_++;
    r.monitor = monitor_id;
    r.edge = edge;
    r.thickness = thickness;
    reservations_[r.id] = r;
    apply_reservations();
    for (auto& [mid, wsid] : active_) do_relayout(wsid);
    refit_sized();
    return ReserveResult{r.id, take()};
}

std::vector<Command> WindowManager::release_edge(ReservationId id) {
    if (!reservations_.erase(id)) return {};
    apply_reservations();
    for (auto& [mid, wsid] : active_) do_relayout(wsid);
    refit_sized();
    return take();
}

std::optional<EdgeReservation> WindowManager::reservation(ReservationId id) const {
    auto it = reservations_.find(id);
    if (it == reservations_.end()) return std::nullopt;
    return it->second;
}

std::vector<EdgeReservation> WindowManager::reservations() const {
    std::vector<EdgeReservation> out;
    for (const auto& [id, r] : reservations_) out.push_back(r);
    return out;
}

Rect WindowManager::work_area(MonitorId monitor_id) const {
    const MonitorSnapshot* m = monitor(monitor_id);
    return m ? m->work_area : Rect{};
}

// ---------------------------------------------------------------- pointer policy

PressDecision WindowManager::classify_press(WindowId id, Point p, uint32_t modifiers,
                                            PressButton button) const {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    const Win& w = it->second;
    const InteractionConfig& ic = config_.interaction;

    if (ic.drag_modifiers & modifiers) {
        if (button == PressButton::Left) return PressDecision{PressAction::Move, 0, true, false};
        if (button == PressButton::Right)
            return PressDecision{PressAction::Resize, resize_edge::Bottom | resize_edge::Right, true, false};
    }
    const Rect& f = w.snap.frame;
    if (button != PressButton::Left || !f.contains(p)) return {};
    // The host draws this window's frame: its title bar and edges are the
    // frame's, outside the client, and a press inside belongs to the client.
    if (insets_now(w) != Margins{}) return {};
    const int32_t lx = p.x - f.x, ly = p.y - f.y;

    if (ic.resize_border > 0 && w.snap.resizable && !w.snap.maximized && !w.snap.fullscreen) {
        const int32_t b = ic.resize_border;
        uint32_t edges = 0;
        if (lx < b) edges |= resize_edge::Left;
        else if (lx >= f.width - b) edges |= resize_edge::Right;
        if (ly < b) edges |= resize_edge::Top;
        else if (ly >= f.height - b) edges |= resize_edge::Bottom;
        if (edges) return PressDecision{PressAction::Resize, edges, true, false};
    }
    if (ic.titlebar_height > 0 && !w.snap.fullscreen && ly < ic.titlebar_height)
        return PressDecision{PressAction::Move, 0, false, true};
    return {};
}

}  // namespace brocompositor
