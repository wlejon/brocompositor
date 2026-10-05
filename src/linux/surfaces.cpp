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

void Server::on_new_surface(wlr_surface* surface) {
    auto r = std::make_unique<SurfaceRec>();
    r->srv = this;
    r->surface = surface;
    r->id = next_surface++;
    r->source = std::make_shared<ClientSurfaceImpl>(this, dispatcher, r->id, surface);
    SurfaceRec* raw = r.get();
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.surfaces[raw->id] = raw->source;
    }
    raw->commit.connect(&surface->events.commit, [this, raw](void*) {
        bool new_buffer = raw->source->on_commit();
        ToplevelRec* top = nullptr;
        LayerRec* layer = nullptr;
        resolve_root(raw->surface, &top, &layer);
        mark_tree_dirty(raw->surface);
        SurfaceCommitted ev;
        ev.surface = raw->id;
        ev.window = top ? top->id : kNoWindow;
        ev.layer = layer ? layer->id : 0;
        ev.new_buffer = new_buffer;
        server_events.push(ev);
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
        if (drag_icon == raw->surface) drag_icon = nullptr;
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

void Server::resolve_root(wlr_surface* surface, ToplevelRec** top, LayerRec** layer) {
    *top = nullptr;
    *layer = nullptr;
    wlr_surface* s = surface;
    for (int depth = 0; s && depth < 64; ++depth) {
        s = wlr_surface_get_root_surface(s);
        if (wlr_xdg_popup* popup = wlr_xdg_popup_try_from_wlr_surface(s)) {
            s = popup->parent;
            continue;
        }
        if (wlr_xdg_toplevel* t = wlr_xdg_toplevel_try_from_wlr_surface(s)) {
            auto it = toplevels.find(t);
            if (it != toplevels.end()) *top = it->second.get();
            return;
        }
        if (wlr_layer_surface_v1* l = wlr_layer_surface_v1_try_from_wlr_surface(s)) {
            auto it = layers.find(l);
            if (it != layers.end()) *layer = it->second.get();
            return;
        }
        return;
    }
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
    if (all_trees_dirty) {
        for (auto& [k, t] : toplevels) tops.insert(t.get());
        for (auto& [k, l] : layers) lays.insert(l.get());
    } else {
        for (wlr_surface* s : dirty_roots) {
            if (!surfaces.count(s)) continue;
            ToplevelRec* t = nullptr;
            LayerRec* l = nullptr;
            resolve_root(s, &t, &l);
            if (t) tops.insert(t);
            if (l) lays.insert(l);
        }
    }
    all_trees_dirty = false;
    dirty_roots.clear();

    std::vector<WindowId> changed;
    for (ToplevelRec* t : tops) {
        if (t->id == kNoWindow) continue;
        wlr_box geo{};
        wlr_xdg_surface_get_geometry(t->xdg->base, &geo);
        auto tree = build_tree(t->xdg->base->surface, Point{geo.x, geo.y}, t->xdg->base, nullptr);
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.windows.find(t->id);
        if (it == mirror.windows.end()) continue;
        if (it->second.tree != tree) {
            it->second.tree = std::move(tree);
            changed.push_back(t->id);
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
