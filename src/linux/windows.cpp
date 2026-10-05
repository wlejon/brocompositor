// Window operations for either shell (xdg_toplevel or managed X11 window):
// the portable commands, activation, hit testing and the initial placement
// policy.
#include "linux/server_impl.h"

#include <algorithm>

namespace brocompositor::wl {

WindowRef Server::window_ref(WindowId id) {
    WindowRef w;
    if (id == kNoWindow) return w;
    w.xdg = toplevel(id);
    if (!w.xdg) w.x = xwindow(id);
    return w;
}

wlr_surface* Server::window_surface(WindowRef w) {
    if (w.xdg) return w.xdg->xdg->base->surface;
#ifdef BC_HAVE_XWAYLAND
    if (w.x && w.x->xs) return w.x->xs->surface;
#endif
    return nullptr;
}

Rect Server::initial_frame(Size g, WindowId parent, Size min) {
    auto area_of = [](OutputRec* out, wlr_output_layout* layout) {
        if (!out || !out->output->enabled) return Rect{};
        if (!out->work_area.empty()) return out->work_area;
        wlr_box b{};
        wlr_output_layout_get_box(layout, out->output, &b);
        return Rect{b.x, b.y, b.width, b.height};
    };
    Rect area = area_of(output_rec(output_at(cursor_x, cursor_y)), layout);
    for (auto it = outputs.begin(); area.empty() && it != outputs.end(); ++it) area = area_of(it->second.get(), layout);
    if (area.empty()) return Rect{0, 0, g.width, g.height};

    // Never larger than the work area (unless the client's minimum is).
    Size s = g;
    if (s.width > area.width) s.width = std::max(area.width, min.width);
    if (s.height > area.height) s.height = std::max(area.height, min.height);

    Point p{area.x + (area.width - s.width) / 2, area.y + (area.height - s.height) / 2};
    // A transient (dialog) opens centred on its parent.
    if (parent != kNoWindow) {
        std::optional<Rect> pf;
        if (ToplevelRec* t = toplevel(parent)) pf = t->snap.frame;
        else if (XwaylandRec* x = xwindow(parent)) pf = x->snap.frame;
        if (pf && !pf->empty()) p = Point{pf->x + (pf->width - s.width) / 2, pf->y + (pf->height - s.height) / 2};
    }
    // Keep it inside the work area; one that still does not fit starts at
    // the work area's top-left corner so its title bar is reachable.
    auto fit = [](int32_t pos, int32_t size, int32_t lo, int32_t len) {
        if (size >= len) return lo;
        return std::clamp(pos, lo, lo + len - size);
    };
    p.x = fit(p.x, s.width, area.x, area.width);
    p.y = fit(p.y, s.height, area.y, area.height);
    return Rect{p.x, p.y, s.width, s.height};
}

bool Server::place_window(WindowId id, const Rect& frame) {
    WindowRef w = window_ref(id);
    if (w.xdg) return place_xdg(*w.xdg, frame);
    if (w.x) return place_xwindow(*w.x, frame);
    return false;
}

bool Server::set_window_visible(WindowId id, bool visible) {
    WindowRef w = window_ref(id);
    if (!w) return false;
    if (w.xdg) set_xdg_visible(*w.xdg, visible);
    else set_xwindow_visible(*w.x, visible);
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.windows.find(id);
        if (it != mirror.windows.end()) it->second.visible = visible;
    }
    schedule_idle_check();
    return true;
}

bool Server::close_window(WindowId id) {
    WindowRef w = window_ref(id);
    if (w.xdg) wlr_xdg_toplevel_send_close(w.xdg->xdg);
    else if (w.x) close_xwindow(*w.x);
    return bool(w);
}

bool Server::set_window_state(WindowId id, bool maximized, bool fullscreen) {
    WindowRef w = window_ref(id);
    if (w.xdg) {
        wlr_xdg_toplevel_set_maximized(w.xdg->xdg, maximized);
        wlr_xdg_toplevel_set_fullscreen(w.xdg->xdg, fullscreen);
    } else if (w.x) {
        set_xwindow_state(*w.x, maximized, fullscreen);
    }
    return bool(w);
}

bool Server::set_window_minimized(WindowId id, bool minimized) {
    WindowRef w = window_ref(id);
    if (w.xdg) {
        // xdg-shell has no minimized state for the client; taskbars see it.
        w.xdg->minimized = minimized;
        publish_window(*w.xdg, change::State);
    } else if (w.x) {
        set_xwindow_minimized(*w.x, minimized);
    }
    return bool(w);
}

void Server::set_window_activated(WindowRef w, bool activated) {
    if (w.xdg) {
        w.xdg->activated = activated;
        wlr_xdg_toplevel_set_activated(w.xdg->xdg, activated);
        foreign_update(w.xdg->foreign.get(), w.xdg->snap, activated, w.xdg->minimized, w.xdg->entered);
    } else if (w.x) {
        activate_xwindow(*w.x, activated);
    }
}

std::optional<SurfaceHit> Server::hit_test_window(WindowId id, double wx, double wy) {
    if (locked()) return std::nullopt;
    WindowRef w = window_ref(id);
    double sx = 0, sy = 0;
    wlr_surface* hit = nullptr;
    if (w.xdg) {
        wlr_box geo{};
        wlr_xdg_surface_get_geometry(w.xdg->xdg->base, &geo);
        hit = wlr_xdg_surface_surface_at(w.xdg->xdg->base, wx + geo.x, wy + geo.y, &sx, &sy);
    } else if (wlr_surface* s = window_surface(w)) {
        hit = wlr_surface_surface_at(s, wx, wy, &sx, &sy);
    }
    if (!hit) return std::nullopt;
    return SurfaceHit{surface_id(hit), sx, sy};
}

void Server::update_all_window_outputs() {
    for (auto& [k, t] : toplevels) update_window_outputs(*t);
#ifdef BC_HAVE_XWAYLAND
    for (auto& [k, x] : xsurfaces) update_xwindow_outputs(*x);
#endif
}

void Server::push_window_request(WindowId id, WindowRequestKind kind, bool foreign) {
    if (id == kNoWindow) return;
    WindowRequest r;
    r.window = id;
    r.kind = kind;
    r.foreign = foreign;
    server_events.push(r);
}

}  // namespace brocompositor::wl
