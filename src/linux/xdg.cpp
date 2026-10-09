// xdg-shell: toplevels become portable windows (map -> WindowAdded), popups
// join their parent's surface tree; xdg-decoration and xdg-activation.
#include "linux/client_surface.h"
#include "linux/server_impl.h"

#include <cmath>

namespace brocompositor::wl {

namespace {

constexpr uint32_t kAllWmCaps = WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
                                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
                                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;

Size geometry_size(wlr_xdg_toplevel* t) {
    wlr_box geo{};
    wlr_xdg_surface_get_geometry(t->base, &geo);
    return Size{geo.width, geo.height};
}

}  // namespace

ToplevelRec* Server::toplevel(WindowId id) {
    if (id == kNoWindow) return nullptr;
    for (auto& [k, t] : toplevels)
        if (t->id == id) return t.get();
    return nullptr;
}

Point Server::root_origin(ToplevelRec& t) {
    wlr_box geo{};
    wlr_xdg_surface_get_geometry(t.xdg->base, &geo);
    return Point{t.pos.x - geo.x, t.pos.y - geo.y};
}

WindowSnapshot Server::snapshot(ToplevelRec& t) {
    WindowSnapshot s;
    s.id = t.id;
    s.native = surface_id(t.xdg->base->surface);
    if (t.xdg->parent) {
        auto it = toplevels.find(t.xdg->parent);
        if (it != toplevels.end()) s.owner = it->second->id;
    }
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(wl_resource_get_client(t.xdg->resource), &pid, &uid, &gid);
    s.process_id = uint32_t(pid);
    s.title = t.xdg->title ? t.xdg->title : "";
    s.app_id = t.xdg->app_id ? t.xdg->app_id : "";
    s.class_name = "xdg_toplevel";
    Size g = geometry_size(t.xdg);
    s.frame = Rect{t.pos.x, t.pos.y, g.width, g.height};
    // Monitor: the output with the largest overlap (else the nearest).
    int64_t best = -1;
    for (auto& [id, out] : outputs) {
        wlr_box ob{};
        wlr_output_layout_get_box(layout, out->output, &ob);
        Rect r{ob.x, ob.y, ob.width, ob.height};
        int64_t a = r.intersected(s.frame).area();
        if (a > best) {
            best = a;
            s.monitor = id;
            s.dpi = uint32_t(std::lround(96.0 * out->output->scale));
        }
    }
    s.maximized = t.xdg->current.maximized;
    s.fullscreen = t.xdg->current.fullscreen;
    s.minimized = false;
    s.decorated = t.ssd;
    const auto& c = t.xdg->current;
    s.resizable = !(c.min_width > 0 && c.min_width == c.max_width && c.min_height > 0 && c.min_height == c.max_height);
    return s;
}

void Server::publish_window(ToplevelRec& t, uint32_t changes) {
    if (t.id == kNoWindow) return;
    WindowSnapshot s = snapshot(t);
    s.minimized = t.minimized;
    if (s.monitor != t.snap.monitor) changes |= change::Monitor;
    bool differs = !(s == t.snap);
    t.snap = s;
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.windows.find(t.id);
        if (it != mirror.windows.end()) it->second.snap = s;
    }
    foreign_update(t.foreign.get(), s, t.activated, t.minimized, t.entered);
    if (differs || changes) events.push(WindowChanged{s, changes});
}

void Server::update_window_outputs(ToplevelRec& t) {
    if (t.id == kNoWindow) return;
    Size g = geometry_size(t.xdg);
    Rect frame{t.pos.x, t.pos.y, g.width, g.height};
    std::set<wlr_output*> now;
    double scale = 0;
    for (auto& [id, out] : outputs) {
        if (!out->output->enabled) continue;
        wlr_box ob{};
        wlr_output_layout_get_box(layout, out->output, &ob);
        if (Rect{ob.x, ob.y, ob.width, ob.height}.intersects(frame)) {
            now.insert(out->output);
            scale = std::max(scale, double(out->output->scale));
        }
    }
    struct Ctx {
        std::set<wlr_output*>* add;
        std::set<wlr_output*>* remove;
        double scale;
    };
    std::set<wlr_output*> add, remove;
    for (wlr_output* o : now)
        if (!t.entered.count(o)) add.insert(o);
    for (wlr_output* o : t.entered)
        if (!now.count(o)) remove.insert(o);
    Ctx ctx{&add, &remove, scale > 0 ? scale : 1.0};
    wlr_xdg_surface_for_each_surface(
        t.xdg->base,
        [](wlr_surface* s, int, int, void* data) {
            auto* c = static_cast<Ctx*>(data);
            for (wlr_output* o : *c->remove) wlr_surface_send_leave(s, o);
            for (wlr_output* o : *c->add) wlr_surface_send_enter(s, o);
            wlr_fractional_scale_v1_notify_scale(s, c->scale);
            wlr_surface_set_preferred_buffer_scale(s, int32_t(std::ceil(c->scale)));
        },
        &ctx);
    t.entered = std::move(now);
    foreign_update(t.foreign.get(), t.snap, t.activated, t.minimized, t.entered);
}

void Server::on_new_toplevel(wlr_xdg_toplevel* xdg) {
    auto rec_ptr = std::make_unique<ToplevelRec>();
    ToplevelRec* t = rec_ptr.get();
    t->srv = this;
    t->xdg = xdg;
    toplevels[xdg] = std::move(rec_ptr);
    wlr_surface* surface = xdg->base->surface;

    t->commit.connect(&surface->events.commit, [this, t](void*) {
        wlr_xdg_toplevel* x = t->xdg;
        if (x->base->initial_commit) {
            wlr_xdg_toplevel_set_wm_capabilities(x, kAllWmCaps);
            wlr_output* o = output_at(mirror.cursor_x, mirror.cursor_y);
            if (OutputRec* out = output_rec(o)) {
                if (!out->work_area.empty())
                    wlr_xdg_toplevel_set_bounds(x, out->work_area.width, out->work_area.height);
            }
            apply_decoration(*t);
            wlr_xdg_toplevel_set_size(x, 0, 0);
            return;
        }
        if (t->id == kNoWindow) return;
        Size g = geometry_size(x);
        uint32_t changes = 0;
        // A placement that waited for this configure lands with it.
        if (!t->pending_pos.empty()) {
            const uint32_t acked = x->base->current.configure_serial;
            size_t landed = 0;
            while (landed < t->pending_pos.size() &&
                   static_cast<int32_t>(acked - t->pending_pos[landed].first) >= 0)
                ++landed;
            if (landed > 0) {
                const Point to = t->pending_pos[landed - 1].second;
                t->pending_pos.erase(t->pending_pos.begin(), t->pending_pos.begin() + landed);
                if (!(to == t->pos)) {
                    t->pos = to;
                    changes |= change::Geometry;
                    mark_tree_dirty(x->base->surface);
                }
            }
        }
        if (!(g == t->last_geometry)) {
            t->last_geometry = g;
            changes |= change::Geometry;
        }
        if (x->current.maximized != t->snap.maximized || x->current.fullscreen != t->snap.fullscreen)
            changes |= change::State;
        if (changes) {
            publish_window(*t, changes);
            update_window_outputs(*t);
        }
    });

    t->map.connect(&surface->events.map, [this, t](void*) {
        t->id = next_window++;
        Size g = geometry_size(t->xdg);
        t->last_geometry = g;
        if (!t->positioned) {
            WindowId parent = kNoWindow;
            if (t->xdg->parent) {
                auto it = toplevels.find(t->xdg->parent);
                if (it != toplevels.end()) parent = it->second->id;
            }
            Size min{t->xdg->current.min_width, t->xdg->current.min_height};
            Rect f = initial_frame(g, parent, min);
            t->pos = Point{f.x, f.y};
            // Larger than the work area: ask the client to shrink to it (the
            // frame stays at the work area's corner until it does).
            if (f.width != g.width || f.height != g.height) wlr_xdg_toplevel_set_size(t->xdg, f.width, f.height);
            t->positioned = true;
        }
        t->visible = true;
        t->snap = snapshot(*t);
        {
            std::lock_guard<std::mutex> lock(mirror.m);
            WindowMirror m;
            m.snap = t->snap;
            m.visible = true;
            m.root = t->snap.native;
            m.ssd = t->ssd;
            mirror.windows[t->id] = m;
        }
        events.push(WindowAdded{t->snap});
        foreign_map(t->foreign, t->id);
        update_window_outputs(*t);
        mark_tree_dirty(t->xdg->base->surface);
        schedule_idle_check();
    });

    t->unmap.connect(&surface->events.unmap, [this, t](void*) {
        if (t->id == kNoWindow) return;
        WindowId id = t->id;
        t->id = kNoWindow;
        t->entered.clear();
        t->positioned = false;
        foreign_unmap(t->foreign);
        {
            std::lock_guard<std::mutex> lock(mirror.m);
            mirror.windows.erase(id);
        }
        if (focused_window == id) {
            focused_window = kNoWindow;
            if (!locked()) wlr_seat_keyboard_notify_clear_focus(seat);
        }
        events.push(WindowRemoved{id});
        schedule_idle_check();
        notify_window_commit(id);  // window captures waiting on it fail (stopped)
    });

    t->destroy.connect(&xdg->events.destroy, [this, xdg](void*) {
        if (auto it = toplevels.find(xdg); it != toplevels.end()) foreign_unmap(it->second->foreign);
        toplevels.erase(xdg);
        mark_all_trees_dirty();
    });

    auto request = [this, t](WindowRequestKind kind) {
        WindowRequest r;
        r.window = t->id;
        r.kind = kind;
        return r;
    };
    t->req_move.connect(&xdg->events.request_move, [this, t, request](void*) {
        if (t->id) server_events.push(request(WindowRequestKind::Move));
    });
    t->req_resize.connect(&xdg->events.request_resize, [this, t, request](void* data) {
        auto* e = static_cast<wlr_xdg_toplevel_resize_event*>(data);
        WindowRequest r = request(WindowRequestKind::Resize);
        r.edges = e->edges;
        if (t->id) server_events.push(r);
    });
    t->req_maximize.connect(&xdg->events.request_maximize, [this, t, request](void*) {
        // The protocol demands a configure in reply; the host decides the state.
        if (t->xdg->base->initialized) wlr_xdg_surface_schedule_configure(t->xdg->base);
        if (t->id)
            server_events.push(request(t->xdg->requested.maximized ? WindowRequestKind::Maximize
                                                                    : WindowRequestKind::Unmaximize));
    });
    t->req_fullscreen.connect(&xdg->events.request_fullscreen, [this, t, request](void*) {
        if (t->xdg->base->initialized) wlr_xdg_surface_schedule_configure(t->xdg->base);
        WindowRequest r = request(t->xdg->requested.fullscreen ? WindowRequestKind::Fullscreen
                                                               : WindowRequestKind::Unfullscreen);
        if (OutputRec* o = output_rec(t->xdg->requested.fullscreen_output)) r.monitor = o->id;
        if (t->id) server_events.push(r);
    });
    t->req_minimize.connect(&xdg->events.request_minimize, [this, t, request](void*) {
        if (t->xdg->base->initialized) wlr_xdg_surface_schedule_configure(t->xdg->base);
        if (t->id) server_events.push(request(WindowRequestKind::Minimize));
    });
    t->req_menu.connect(&xdg->events.request_show_window_menu, [this, t, request](void* data) {
        auto* e = static_cast<wlr_xdg_toplevel_show_window_menu_event*>(data);
        WindowRequest r = request(WindowRequestKind::ShowMenu);
        r.point = Point{e->x, e->y};
        if (t->id) server_events.push(r);
    });
    t->set_title.connect(&xdg->events.set_title, [this, t](void*) { publish_window(*t, change::Title); });
    t->set_app_id.connect(&xdg->events.set_app_id, [this, t](void*) { publish_window(*t, change::Title); });
    t->set_parent.connect(&xdg->events.set_parent, [this, t](void*) { publish_window(*t, 0); });
}

void Server::on_new_popup(wlr_xdg_popup* xdg) {
    auto p = std::make_unique<PopupRec>();
    PopupRec* raw = p.get();
    raw->srv = this;
    raw->xdg = xdg;
    popups[xdg] = std::move(p);

    auto unconstrain = [this, raw] {
        wlr_xdg_popup* popup = raw->xdg;
        if (!popup->parent) return;
        RootRef root = resolve_root(popup->parent);
        ToplevelRec* top = root.top;
        LayerRec* layer = root.layer;
        Point origin;
        if (top && top->id != kNoWindow) origin = root_origin(*top);
        else if (layer) origin = Point{layer->rect.x, layer->rect.y};
        else return;
        wlr_output* o = output_at(origin.x + 1, origin.y + 1);
        if (!o && !outputs.empty()) o = outputs.begin()->second->output;
        if (!o) return;
        wlr_box ob{};
        wlr_output_layout_get_box(layout, o, &ob);
        wlr_box box{ob.x - origin.x, ob.y - origin.y, ob.width, ob.height};
        wlr_xdg_popup_unconstrain_from_box(popup, &box);
    };
    raw->commit.connect(&xdg->base->surface->events.commit, [raw, unconstrain](void*) {
        if (raw->xdg->base->initial_commit) {
            unconstrain();
            wlr_xdg_surface_schedule_configure(raw->xdg->base);
        }
    });
    raw->reposition.connect(&xdg->events.reposition, [unconstrain](void*) { unconstrain(); });
    raw->destroy.connect(&xdg->events.destroy, [this, xdg](void*) {
        popups.erase(xdg);
        mark_all_trees_dirty();
    });
}

void Server::on_new_decoration(wlr_xdg_toplevel_decoration_v1* d) {
    auto it = toplevels.find(d->toplevel);
    if (it == toplevels.end()) return;
    ToplevelRec* t = it->second.get();
    t->decoration = d;
    t->deco_request.connect(&d->events.request_mode, [this, t](void*) { apply_decoration(*t); });
    t->deco_destroy.connect(&d->events.destroy, [this, t](void*) {
        t->decoration = nullptr;
        t->deco_request.disconnect();
        t->deco_destroy.disconnect();
        apply_decoration(*t);  // back to client-side
    });
    apply_decoration(*t);
}

// Negotiates the xdg-decoration mode (the client's request wins over the
// server preference) and publishes whether the host should draw the frame.
void Server::apply_decoration(ToplevelRec& t) {
    bool ssd = false;
    if (t.decoration && t.xdg->base->initialized) {
        auto mode = config.prefer_server_side_decorations ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                          : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
        if (t.decoration->requested_mode != WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE)
            mode = t.decoration->requested_mode;
        wlr_xdg_toplevel_decoration_v1_set_mode(t.decoration, mode);
        ssd = mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    }
    if (ssd == t.ssd) return;
    t.ssd = ssd;
    if (t.id == kNoWindow) return;  // published at map
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.windows.find(t.id);
        if (it != mirror.windows.end()) it->second.ssd = ssd;
    }
    publish_window(t, change::State);
}

void Server::on_activation_request(wlr_xdg_activation_v1_request_activate_event* e) {
    // The token (wlroots checked its seat / serial when the requester gave
    // them) names any window: xdg toplevels and X11 windows alike.
    WindowId id = resolve_root(e->surface).window();
    if (id == kNoWindow) return;
    push_window_request(id, WindowRequestKind::Activate, false);
}

bool Server::place_xdg(ToplevelRec& tr, const Rect& frame) {
    ToplevelRec* t = &tr;
    const Point to{frame.x, frame.y};
    t->positioned = true;
    Size g = geometry_size(t->xdg);
    const bool resize = frame.width > 0 && frame.height > 0 &&
                        (frame.width != g.width || frame.height != g.height ||
                         t->xdg->scheduled.width != frame.width || t->xdg->scheduled.height != frame.height);
    if (resize && t->xdg->base->initialized && t->id != kNoWindow) {
        // Moved when the client has drawn the new size (the commit acking
        // this configure), not before.
        const uint32_t serial = wlr_xdg_toplevel_set_size(t->xdg, frame.width, frame.height);
        if (!t->pending_pos.empty() && t->pending_pos.back().first == serial) t->pending_pos.back().second = to;
        else t->pending_pos.emplace_back(serial, to);
    } else {
        if (resize) wlr_xdg_toplevel_set_size(t->xdg, frame.width, frame.height);
        t->pending_pos.clear();
        t->pos = to;
    }
    publish_window(*t, change::Geometry);
    update_window_outputs(*t);
    mark_tree_dirty(t->xdg->base->surface);
    return true;
}

void Server::set_xdg_visible(ToplevelRec& t, bool visible) {
    t.visible = visible;
    if (t.xdg->base->client->shell->version >= 6) wlr_xdg_toplevel_set_suspended(t.xdg, !visible);
    if (!visible) {
        // A hidden window gets no more frame callbacks, and a Vulkan FIFO
        // swapchain waits in its next present for the one it has asked for,
        // never reading the suspended state. That callback is answered once,
        // from an idle source added after the configure's (wlroots sends the
        // configure from idle too), so the client has the suspended state in
        // hand when its present returns and can stop presenting.
        struct Pending {
            Server* srv;
            wlr_xdg_toplevel* xdg;
        };
        wl_event_loop_add_idle(
            wl_display_get_event_loop(display),
            [](void* data) {
                std::unique_ptr<Pending> p(static_cast<Pending*>(data));
                auto it = p->srv->toplevels.find(p->xdg);
                if (it == p->srv->toplevels.end() || it->second->visible) return;
                wlr_xdg_surface_for_each_surface(
                    p->xdg->base,
                    [](wlr_surface* s, int, int, void* srv) { static_cast<Server*>(srv)->send_frame_done(s, 0); },
                    p->srv);
            },
            new Pending{this, t.xdg});
    }
}

}  // namespace brocompositor::wl
