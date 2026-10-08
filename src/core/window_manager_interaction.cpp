// What a host that composites the desktop itself asks of the core beyond
// placement: the stacking order, the band its window frames take
// (DecorationConfig), interactive move / resize, and snapping a window to an
// edge of its monitor's work area (window_manager.h).
#include "brocompositor/window_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace brocompositor {

// ---------------------------------------------------------------- names

const char* to_string(SnapZone zone) {
    switch (zone) {
        case SnapZone::None: return "none";
        case SnapZone::Maximize: return "maximize";
        case SnapZone::Left: return "left";
        case SnapZone::Right: return "right";
        case SnapZone::TopLeft: return "top-left";
        case SnapZone::TopRight: return "top-right";
        case SnapZone::BottomLeft: return "bottom-left";
        case SnapZone::BottomRight: return "bottom-right";
    }
    return "none";
}

std::optional<SnapZone> snap_zone_from_string(const std::string& name) {
    for (uint32_t i = 0; i <= uint32_t(SnapZone::BottomRight); ++i)
        if (name == to_string(SnapZone(i))) return SnapZone(i);
    if (name == "maximized" || name == "top") return SnapZone::Maximize;
    return std::nullopt;
}

// ---------------------------------------------------------------- decorations

Margins WindowManager::insets_for(const Win& w, bool maximized) const {
    if (!w.snap.decorated) return {};
    return maximized ? config_.decoration.maximized_insets : config_.decoration.insets;
}

Margins WindowManager::insets_now(const Win& w) const {
    if (w.snap.fullscreen || w.sized == WindowState::Fullscreen) return {};
    return insets_for(w, w.snap.maximized || w.sized == WindowState::Maximized);
}

Rect WindowManager::client_in(const Win& w, const Rect& outer, bool maximized) const {
    if (outer.empty()) return outer;
    const Margins in = insets_for(w, maximized);
    return Rect{outer.x + in.left, outer.y + in.top, std::max(1, outer.width - in.horizontal()),
                std::max(1, outer.height - in.vertical())};
}

std::vector<Command> WindowManager::set_decoration(const DecorationConfig& config) {
    if (config == config_.decoration) return {};
    config_.decoration = config;
    for (auto& [id, w] : windows_) {
        if (w.snap.decorated) w.placed.reset();
    }
    refit_sized();
    for (auto& [mid, wsid] : active_) do_relayout(wsid);
    return take();
}

Margins WindowManager::decoration_insets(WindowId id) const {
    auto it = windows_.find(id);
    return it == windows_.end() ? Margins{} : insets_now(it->second);
}

// ---------------------------------------------------------------- stacking

void WindowManager::stack_add(WindowId id) {
    if (std::find(stack_.begin(), stack_.end(), id) != stack_.end()) return;
    stack_.push_back(id);
    ++stack_serial_;
}

void WindowManager::stack_remove(WindowId id) {
    auto it = std::find(stack_.begin(), stack_.end(), id);
    if (it == stack_.end()) return;
    stack_.erase(it);
    ++stack_serial_;
}

// The window goes on top with the windows it owns (transitively) above it,
// each group keeping its own relative order.
bool WindowManager::stack_raise(WindowId id) {
    if (std::find(stack_.begin(), stack_.end(), id) == stack_.end()) return false;
    std::set<WindowId> group{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [wid, w] : windows_) {
            if (group.count(wid) || w.snap.owner == kNoWindow || !group.count(w.snap.owner)) continue;
            group.insert(wid);
            grew = true;
        }
    }
    std::vector<WindowId> next;
    next.reserve(stack_.size());
    for (WindowId w : stack_)
        if (!group.count(w)) next.push_back(w);
    next.push_back(id);
    for (WindowId w : stack_)
        if (w != id && group.count(w)) next.push_back(w);
    if (next == stack_) return false;
    stack_ = std::move(next);
    ++stack_serial_;
    return true;
}

bool WindowManager::raise(WindowId id) { return stack_raise(id); }

// ---------------------------------------------------------------- snapping

Rect WindowManager::snap_rect(MonitorId monitor_id, SnapZone zone) const {
    const MonitorSnapshot* m = monitor(monitor_id);
    if (!m || zone == SnapZone::None) return {};
    const Rect& wa = m->work_area;
    if (wa.empty()) return {};
    const int32_t lw = wa.width / 2, rw = wa.width - lw;
    const int32_t th = wa.height / 2, bh = wa.height - th;
    switch (zone) {
        case SnapZone::None: return {};
        case SnapZone::Maximize: return wa;
        case SnapZone::Left: return Rect{wa.x, wa.y, lw, wa.height};
        case SnapZone::Right: return Rect{wa.x + lw, wa.y, rw, wa.height};
        case SnapZone::TopLeft: return Rect{wa.x, wa.y, lw, th};
        case SnapZone::TopRight: return Rect{wa.x + lw, wa.y, rw, th};
        case SnapZone::BottomLeft: return Rect{wa.x, wa.y + th, lw, bh};
        case SnapZone::BottomRight: return Rect{wa.x + lw, wa.y + th, rw, bh};
    }
    return {};
}

void WindowManager::place_snapped(WindowId id, Win& w) {
    const MonitorSnapshot* m = window_monitor(w);
    if (!m) return;
    const Rect client = client_in(w, snap_rect(m->id, w.snapped), false);
    if (client.empty() || w.placed == client) return;
    w.placed = client;
    emit(PlaceWindow{id, client});
}

std::vector<Command> WindowManager::snap(WindowId id, SnapZone zone) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    Win& w = it->second;
    if (w.snap.fullscreen || w.sized == WindowState::Fullscreen) return {};
    if (zone == SnapZone::None) return restore(id);
    if (zone == SnapZone::Maximize) return maximize(id);
    if (!w.snap.resizable) return {};
    if (w.snapped == zone && !w.snap.maximized && !w.snap.minimized) return {};

    if (!w.restore_rect && !w.snap.maximized && !w.snap.frame.empty()) w.restore_rect = w.snap.frame;
    if (w.snap.maximized || w.sized == WindowState::Maximized || w.snap.minimized) {
        // Snapping leaves the maximized (or minimized) state; the frame the
        // window had before either is kept for a later restore.
        w.sized = WindowState::Normal;
        emit(SetWindowState{id, WindowState::Normal});
    }
    w.snapped = zone;
    w.placed.reset();
    place_snapped(id, w);
    do_relayout(w.ws);  // a tiled workspace closes the gap it left
    return take();
}

std::vector<Command> WindowManager::snap_toward(WindowId id, Direction dir) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    const Win& w = it->second;
    if (w.snap.fullscreen || w.snap.minimized) return {};
    const SnapZone cur = w.snap.maximized ? SnapZone::Maximize : w.snapped;
    switch (dir) {
        case Direction::Left:
            if (cur == SnapZone::Right) return snap(id, SnapZone::None);
            return snap(id, SnapZone::Left);
        case Direction::Right:
            if (cur == SnapZone::Left) return snap(id, SnapZone::None);
            return snap(id, SnapZone::Right);
        case Direction::Up:
            return snap(id, SnapZone::Maximize);
        case Direction::Down:
            if (cur != SnapZone::None) return snap(id, SnapZone::None);
            return minimize(id);
    }
    return {};
}

// The snap a dragged pointer arms: within edge_threshold of an edge of the
// monitor under it that no other monitor continues past.
SnapZone WindowManager::zone_at(Point p) const {
    const SnapConfig& sc = config_.snap;
    if (!sc.enabled) return SnapZone::None;
    const MonitorSnapshot* m = monitor(monitor_at(p));
    if (!m) return SnapZone::None;
    const Rect& b = m->bounds;
    const int32_t t = std::max(1, sc.edge_threshold);
    const int32_t c = std::max(0, sc.corner_size);
    const bool left = p.x < b.x + t && monitor_at(Point{b.x - 1, p.y}) == kNoMonitor;
    const bool right = p.x >= b.right() - t && monitor_at(Point{b.right(), p.y}) == kNoMonitor;
    const bool top = p.y < b.y + t && monitor_at(Point{p.x, b.y - 1}) == kNoMonitor;
    if (left || right) {
        if (c > 0 && p.y < b.y + c) return left ? SnapZone::TopLeft : SnapZone::TopRight;
        if (c > 0 && p.y >= b.bottom() - c) return left ? SnapZone::BottomLeft : SnapZone::BottomRight;
        return left ? SnapZone::Left : SnapZone::Right;
    }
    if (top) return SnapZone::Maximize;
    return SnapZone::None;
}

// ---------------------------------------------------------------- dragging

std::vector<Command> WindowManager::begin_move(WindowId id, Point pointer, bool immediate) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    Win& w = it->second;
    if (w.snap.fullscreen || w.snap.minimized) return {};
    if (drag_ && drag_->info.window != id) moving_.erase(drag_->info.window);
    Drag d;
    d.info.window = id;
    d.info.action = PressAction::Move;
    d.info.active = immediate;
    d.start_pointer = pointer;
    d.start_frame = w.snap.frame;
    drag_ = d;
    moving_.insert(id);
    stack_raise(id);
    if (focused_ != id) emit(FocusWindow{id});
    return take();
}

std::vector<Command> WindowManager::begin_resize(WindowId id, Point pointer, uint32_t edges, bool immediate) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return {};
    Win& w = it->second;
    edges &= resize_edge::Top | resize_edge::Bottom | resize_edge::Left | resize_edge::Right;
    if (!edges || !w.snap.resizable || w.snap.fullscreen || w.snap.minimized || w.snap.maximized) return {};
    if (drag_ && drag_->info.window != id) moving_.erase(drag_->info.window);
    Drag d;
    d.info.window = id;
    d.info.action = PressAction::Resize;
    d.info.edges = edges;
    d.info.active = immediate;
    d.start_pointer = pointer;
    d.start_frame = w.snap.frame;
    drag_ = d;
    moving_.insert(id);
    stack_raise(id);
    if (focused_ != id) emit(FocusWindow{id});
    return take();
}

// A maximized or snapped window dragged by its title bar returns to the size
// it had, under the pointer: the same fraction across the frame, the same
// depth into the title bar.
void WindowManager::unsnap_for_drag(WindowId id, Win& w, Point p) {
    const Rect cur = w.snap.frame;
    const Margins in_now = insets_now(w);
    const Margins in = insets_for(w, false);
    Rect back = w.restore_rect ? *w.restore_rect : w.floating_rect;
    if (back.empty()) back = cur;
    const Rect outer{cur.x - in_now.left, cur.y - in_now.top, cur.width + in_now.horizontal(),
                     cur.height + in_now.vertical()};
    const double fx = outer.width > 0 ? double(p.x - outer.x) / double(outer.width) : 0.5;
    const int32_t outer_w = back.width + in.horizontal();
    const int32_t grab_y = drag_->start_pointer.y - outer.y;  // depth of the press into the frame
    const int32_t ox = p.x - int32_t(std::lround(std::clamp(fx, 0.0, 1.0) * outer_w));
    const int32_t oy = p.y - std::max(0, grab_y);
    const Rect next{ox + in.left, oy + in.top, back.width, back.height};

    if (w.snap.maximized || w.sized == WindowState::Maximized) emit(SetWindowState{id, WindowState::Normal});
    w.sized = WindowState::Normal;
    w.snapped = SnapZone::None;
    w.restore_rect.reset();
    if (w.floating) w.floating_rect = next;
    w.placed = next;
    emit(PlaceWindow{id, next});
    drag_->start_frame = next;
    drag_->start_pointer = p;
}

std::vector<Command> WindowManager::drag_to(Point p) {
    if (!drag_) return {};
    auto it = windows_.find(drag_->info.window);
    if (it == windows_.end()) {
        drag_.reset();
        return {};
    }
    const WindowId id = it->first;
    Win& w = it->second;
    DragInfo& info = drag_->info;
    const InteractionConfig& ic = config_.interaction;

    if (!info.active) {
        const double dist = std::hypot(double(p.x - drag_->start_pointer.x), double(p.y - drag_->start_pointer.y));
        if (dist < double(std::max(0, ic.drag_threshold))) return {};
        info.active = true;
        const bool sized = w.snap.maximized || w.sized == WindowState::Maximized || w.snapped != SnapZone::None;
        if (info.action == PressAction::Move && sized) {
            if (!config_.snap.restore_on_drag) return take();
            unsnap_for_drag(id, w, p);
        } else if (info.action == PressAction::Resize && w.snapped != SnapZone::None) {
            // Resizing a snapped window keeps the new size as its own.
            w.snapped = SnapZone::None;
            w.restore_rect.reset();
        }
    }
    if (info.action == PressAction::Move && (w.snap.maximized || w.sized == WindowState::Maximized)) return take();

    const int32_t dx = p.x - drag_->start_pointer.x, dy = p.y - drag_->start_pointer.y;
    Rect f = drag_->start_frame;
    if (info.action == PressAction::Move) {
        f.x += dx;
        f.y += dy;
        // The title bar stays reachable: the frame's top never goes above
        // the top of the monitor under the pointer.
        MonitorId mid = monitor_at(p);
        if (const MonitorSnapshot* m = monitor(mid != kNoMonitor ? mid : w.snap.monitor)) {
            const int32_t top = m->bounds.y + insets_now(w).top;
            if (f.y < top) f.y = top;
        }
        const bool may_snap = w.snap.resizable && !is_tiled(w);
        info.snap = may_snap ? zone_at(p) : SnapZone::None;
        info.snap_rect = snap_rect(monitor_at(p), info.snap);
    } else {
        const uint32_t e = info.edges;
        const Size min{std::max(1, ic.min_size.width), std::max(1, ic.min_size.height)};
        const Rect s = drag_->start_frame;
        if (e & resize_edge::Left) {
            f.x = s.x + dx;
            f.width = s.width - dx;
        }
        if (e & resize_edge::Right) f.width = s.width + dx;
        if (e & resize_edge::Top) {
            f.y = s.y + dy;
            f.height = s.height - dy;
        }
        if (e & resize_edge::Bottom) f.height = s.height + dy;
        if (f.width < min.width) {
            if (e & resize_edge::Left) f.x = s.x + s.width - min.width;
            f.width = min.width;
        }
        if (f.height < min.height) {
            if (e & resize_edge::Top) f.y = s.y + s.height - min.height;
            f.height = min.height;
        }
    }
    if (w.placed != f) {
        w.placed = f;
        if (w.floating && !is_tiled(w)) w.floating_rect = f;
        emit(PlaceWindow{id, f});
    }
    return take();
}

std::vector<Command> WindowManager::end_drag() {
    if (!drag_) return {};
    const Drag d = *drag_;
    drag_.reset();
    const WindowId id = d.info.window;
    auto it = windows_.find(id);
    if (it == windows_.end()) {
        moving_.erase(id);
        return take();
    }
    Win& w = it->second;
    if (d.info.active && d.info.action == PressAction::Move && d.info.snap != SnapZone::None) {
        moving_.erase(id);
        // Snapped where the pointer let go: that monitor's workspace takes it.
        MonitorId mid = kNoMonitor;
        for (const auto& m : monitors_)
            if (m.work_area.contains(d.info.snap_rect) || m.bounds.intersects(d.info.snap_rect)) {
                mid = m.id;
                break;
            }
        auto ws = workspaces_.find(w.ws);
        if (mid != kNoMonitor && ws != workspaces_.end() && ws->second.monitor != mid && active_.count(mid)) {
            WorkspaceId from = w.ws;
            detach(id, w);
            attach(id, w, active_[mid]);
            do_relayout(from);
        }
        // Restore returns to the frame it had before the drag, not where it
        // was dropped.
        if (!w.restore_rect && !d.start_frame.empty()) w.restore_rect = d.start_frame;
        return snap(id, d.info.snap);
    }
    if (d.info.active) on_move_size_end(id);
    else moving_.erase(id);
    return take();
}

std::vector<Command> WindowManager::cancel_drag() {
    if (!drag_) return {};
    moving_.erase(drag_->info.window);
    drag_.reset();
    return take();
}

std::optional<DragInfo> WindowManager::drag() const {
    if (!drag_) return std::nullopt;
    return drag_->info;
}

}  // namespace brocompositor
