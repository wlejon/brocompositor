// Surface registry and surface trees: every wl_surface gets a SurfaceId and a
// ClientSurfaceImpl; commits refresh the drawable tree of the window or layer
// surface they belong to (lazily, on an idle callback).
#include "linux/client_surface.h"
#include "linux/server_impl.h"

#include <ctime>

namespace brocompositor::wl {

SurfaceRec* Server::rec(wlr_surface* surface) {
    auto it = surfaces.find(surface);
    return it == surfaces.end() ? nullptr : it->second.get();
}

SurfaceId Server::surface_id(wlr_surface* surface) {
    SurfaceRec* r = surface ? rec(surface) : nullptr;
    return r ? r->id : kNoSurface;
}

WindowId RootRef::window() const {
    if (top) return top->id;
    if (x) return x->id;
    return kNoWindow;
}

wlr_surface* Server::surface_by_id(SurfaceId id) {
    auto it = surface_ids.find(id);
    return it == surface_ids.end() ? nullptr : it->second;
}

void Server::on_new_surface(wlr_surface* surface) {
    auto r = std::make_unique<SurfaceRec>();
    r->srv = this;
    r->surface = surface;
    r->id = next_surface++;
    r->source = std::make_shared<ClientSurfaceImpl>(this, dispatcher, lock_gate, r->id, surface);
    SurfaceRec* raw = r.get();
    surface_ids[raw->id] = surface;
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.surfaces[raw->id] = raw->source;
    }
    raw->commit.connect(&surface->events.commit, [this, raw](void*) {
        bool new_buffer = raw->source->on_commit();
        RootRef root = resolve_root(raw->surface);
        mark_tree_dirty(raw->surface);
        SurfaceCommitted ev;
        ev.surface = raw->id;
        ev.window = root.window();
        ev.layer = root.layer ? root.layer->id : 0;
        ev.unmanaged = root.x ? root.x->unmanaged : 0;
        ev.lock = root.lock ? root.lock->output : kNoMonitor;
        ev.new_buffer = new_buffer;
        server_events.push(ev);
        if (ev.window != kNoWindow && new_buffer) notify_window_commit(ev.window);
        // Any visible change wants a frame; the host decides what to draw.
        for (auto& [id, out] : outputs)
            if (out->output->enabled) wlr_output_schedule_frame(out->output);
    });
    raw->new_subsurface.connect(&surface->events.new_subsurface, [this](void*) { mark_all_trees_dirty(); });
    raw->destroy.connect(&surface->events.destroy, [this, raw](void*) {
        forget_surface_focus(raw->surface);
        raw->source->on_destroy();
        {
            std::lock_guard<std::mutex> lock(mirror.m);
            mirror.surfaces.erase(raw->id);
        }
        surface_ids.erase(raw->id);
        if (drag_icon == raw->surface) drag_icon = nullptr;
        if (text_focus == raw->surface) text_focus = nullptr;
        dirty_roots.erase(raw->surface);
        mark_all_trees_dirty();
        surfaces.erase(raw->surface);  // destroys raw (and its listeners)
    });
    surfaces[surface] = std::move(r);
}

void Server::send_frame_done(wlr_surface* surface, int64_t timestamp_ns) {
    timespec ts{};
    if (timestamp_ns > 0) {
        ts.tv_sec = time_t(timestamp_ns / 1000000000);
        ts.tv_nsec = long(timestamp_ns % 1000000000);
    } else {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    wlr_surface_send_frame_done(surface, &ts);
}

RootRef Server::resolve_root(wlr_surface* surface) {
    RootRef r;
    wlr_surface* s = surface;
    for (int depth = 0; s && depth < 64; ++depth) {
        s = wlr_surface_get_root_surface(s);
        if (wlr_xdg_popup* popup = wlr_xdg_popup_try_from_wlr_surface(s)) {
            s = popup->parent;
            continue;
        }
        if (wlr_input_popup_surface_v2_try_from_wlr_surface(s)) {
            // Input-method popups join the tree of the surface being typed into.
            wlr_text_input_v3* ti = active_text_input();
            if (!ti || !ti->focused_surface) return r;
            s = ti->focused_surface;
            continue;
        }
        if (wlr_xdg_toplevel* t = wlr_xdg_toplevel_try_from_wlr_surface(s)) {
            auto it = toplevels.find(t);
            if (it != toplevels.end()) r.top = it->second.get();
            return r;
        }
        if (wlr_layer_surface_v1* l = wlr_layer_surface_v1_try_from_wlr_surface(s)) {
            auto it = layers.find(l);
            if (it != layers.end()) r.layer = it->second.get();
            return r;
        }
        if (wlr_session_lock_surface_v1* ls = wlr_session_lock_surface_v1_try_from_wlr_surface(s)) {
            auto it = lock_surfaces.find(ls);
            if (it != lock_surfaces.end()) r.lock = it->second.get();
            return r;
        }
        r.x = xrec_of(s);
        return r;
    }
    return r;
}

namespace {

struct TreeCollect {
    Server* srv;
    std::vector<SurfaceNode>* out;
    Point origin;  // subtracted from iterator coordinates
    bool popup;
};

void collect(wlr_surface* s, int sx, int sy, void* data) {
    auto* c = static_cast<TreeCollect*>(data);
    if (!s->mapped) return;
    SurfaceNode n;
    n.surface = c->srv->surface_id(s);
    n.offset = Point{sx - c->origin.x, sy - c->origin.y};
    n.size = Size{s->current.width, s->current.height};
    n.popup = c->popup;
    if (n.surface != kNoSurface) c->out->push_back(n);
}

}  // namespace

std::vector<SurfaceNode> Server::build_tree(wlr_surface* root, Point origin, wlr_xdg_surface* xdg_root,
                                            wlr_layer_surface_v1* layer_root) {
    std::vector<SurfaceNode> out;
    TreeCollect c{this, &out, origin, false};
    wlr_surface_for_each_surface(root, collect, &c);
    c.popup = true;
    if (xdg_root) wlr_xdg_surface_for_each_popup_surface(xdg_root, collect, &c);
    if (layer_root) wlr_layer_surface_v1_for_each_popup_surface(layer_root, collect, &c);
    return out;
}

void Server::mark_tree_dirty(wlr_surface* surface) {
    dirty_roots.insert(surface);
    if (!tree_idle)
        tree_idle = wl_event_loop_add_idle(
            loop, [](void* data) { static_cast<Server*>(data)->refresh_trees(); }, this);
}

void Server::mark_all_trees_dirty() {
    all_trees_dirty = true;
    if (!tree_idle)
        tree_idle = wl_event_loop_add_idle(
            loop, [](void* data) { static_cast<Server*>(data)->refresh_trees(); }, this);
}

void Server::refresh_trees() {
    tree_idle = nullptr;
    std::set<ToplevelRec*> tops;
    std::set<LayerRec*> lays;
    std::set<XwaylandRec*> xs;
    bool lock_dirty = false;
    if (all_trees_dirty) {
        for (auto& [k, t] : toplevels) tops.insert(t.get());
        for (auto& [k, l] : layers) lays.insert(l.get());
#ifdef BC_HAVE_XWAYLAND
        for (auto& [k, x] : xsurfaces) xs.insert(x.get());
#endif
        lock_dirty = true;
    } else {
        for (wlr_surface* s : dirty_roots) {
            if (!surfaces.count(s)) continue;
            RootRef r = resolve_root(s);
            if (r.top) tops.insert(r.top);
            if (r.layer) lays.insert(r.layer);
            if (r.x) xs.insert(r.x);
            if (r.lock) lock_dirty = true;
        }
    }
    all_trees_dirty = false;
    dirty_roots.clear();
    if (lock_dirty) refresh_lock_trees();

    std::vector<std::pair<WindowId, std::vector<SurfaceNode>>> trees;
    for (ToplevelRec* t : tops) {
        if (t->id == kNoWindow) continue;
        wlr_box geo{};
        wlr_xdg_surface_get_geometry(t->xdg->base, &geo);
        trees.emplace_back(t->id, build_tree(t->xdg->base->surface, Point{geo.x, geo.y}, t->xdg->base, nullptr));
    }
    for (XwaylandRec* x : xs)
        if (x->id != kNoWindow) trees.emplace_back(x->id, xwindow_tree(*x));
    std::vector<WindowId> changed;
    for (auto& [id, tree] : trees) {
        append_im_popups(id, tree);
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.windows.find(id);
        if (it == mirror.windows.end()) continue;
        if (it->second.tree != tree) {
            it->second.tree = std::move(tree);
            changed.push_back(id);
        }
    }
    for (WindowId id : changed) server_events.push(WindowTreeChanged{id});
    for (LayerRec* l : lays) {
        if (!l->mapped) continue;
        auto tree = build_tree(l->layer->surface, Point{0, 0}, nullptr, l->layer);
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.layers.find(l->id);
        if (it != mirror.layers.end()) it->second.tree = std::move(tree);
    }
}

}  // namespace brocompositor::wl
