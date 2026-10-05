// wlr-layer-shell: panels, docks, wallpapers, lock screens. Layer surfaces
// are arranged per output; exclusive zones (and the host's own edge
// reservations) shrink the output's work area, which reaches the portable
// core as MonitorsChanged, and each exclusive zone is a ReservationChanged.
#include "linux/server_impl.h"

#include <algorithm>

namespace brocompositor::wl {

namespace {

constexpr uint32_t kTop = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
constexpr uint32_t kBottom = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
constexpr uint32_t kLeft = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
constexpr uint32_t kRight = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

// Shrinks `usable` by an exclusive zone anchored to one edge (optionally
// spanning the two perpendicular edges). Returns the reserved strip.
Rect apply_exclusive(Rect& usable, uint32_t anchor, int32_t zone, const wlr_layer_surface_v1_state& st) {
    if (zone <= 0) return {};
    Rect r;
    auto is = [&](uint32_t edge, uint32_t perp) { return anchor == edge || anchor == (edge | perp); };
    if (is(kTop, kLeft | kRight)) {
        int32_t z = zone + st.margin.top;
        r = Rect{usable.x, usable.y, usable.width, z};
        usable.y += z;
        usable.height -= z;
    } else if (is(kBottom, kLeft | kRight)) {
        int32_t z = zone + st.margin.bottom;
        r = Rect{usable.x, usable.bottom() - z, usable.width, z};
        usable.height -= z;
    } else if (is(kLeft, kTop | kBottom)) {
        int32_t z = zone + st.margin.left;
        r = Rect{usable.x, usable.y, z, usable.height};
        usable.x += z;
        usable.width -= z;
    } else if (is(kRight, kTop | kBottom)) {
        int32_t z = zone + st.margin.right;
        r = Rect{usable.right() - z, usable.y, z, usable.height};
        usable.width -= z;
    }
    if (usable.width < 0) usable.width = 0;
    if (usable.height < 0) usable.height = 0;
    return r;
}

Rect place(const Rect& bounds, const wlr_layer_surface_v1_state& st) {
    Rect box{0, 0, int32_t(st.desired_width), int32_t(st.desired_height)};
    uint32_t a = st.anchor;
    if ((a & kLeft) && (a & kRight) && box.width == 0) {
        box.x = bounds.x + st.margin.left;
        box.width = bounds.width - (st.margin.left + st.margin.right);
    } else if (a & kLeft) {
        box.x = bounds.x + st.margin.left;
    } else if (a & kRight) {
        box.x = bounds.right() - box.width - st.margin.right;
    } else {
        box.x = bounds.x + (bounds.width - box.width) / 2;
    }
    if ((a & kTop) && (a & kBottom) && box.height == 0) {
        box.y = bounds.y + st.margin.top;
        box.height = bounds.height - (st.margin.top + st.margin.bottom);
    } else if (a & kTop) {
        box.y = bounds.y + st.margin.top;
    } else if (a & kBottom) {
        box.y = bounds.bottom() - box.height - st.margin.bottom;
    } else {
        box.y = bounds.y + (bounds.height - box.height) / 2;
    }
    if (box.width < 0) box.width = 0;
    if (box.height < 0) box.height = 0;
    return box;
}

}  // namespace

LayerRec* Server::layer_by_id(LayerSurfaceId id) {
    for (auto& [k, l] : layers)
        if (l->id == id) return l.get();
    return nullptr;
}

LayerSurfaceInfo Server::layer_info(LayerRec& l) {
    LayerSurfaceInfo i;
    i.id = l.id;
    if (OutputRec* o = output_rec(l.layer->output)) i.monitor = o->id;
    i.layer = Layer(uint32_t(l.layer->current.layer));
    i.name_space = l.layer->namespace_ ? l.layer->namespace_ : "";
    i.rect = l.rect;
    i.exclusive_zone = l.layer->current.exclusive_zone;
    i.keyboard = KeyboardInteractivity(uint32_t(l.layer->current.keyboard_interactive));
    i.mapped = l.mapped;
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(wl_resource_get_client(l.layer->resource), &pid, &uid, &gid);
    i.process_id = uint32_t(pid);
    i.reservation = l.reservation;
    return i;
}

void Server::arrange_output(OutputRec& out) {
    wlr_box ob{};
    wlr_output_layout_get_box(layout, out.output, &ob);
    Rect full{ob.x, ob.y, ob.width, ob.height};
    Rect usable = full;

    for (auto& [rid, r] : host_reservations) {
        if (r.monitor != out.id) continue;
        int32_t t = std::min(r.thickness, r.edge == Edge::Left || r.edge == Edge::Right ? usable.width : usable.height);
        Rect granted;
        switch (r.edge) {
            case Edge::Top: granted = Rect{usable.x, usable.y, usable.width, t}; usable.y += t; usable.height -= t; break;
            case Edge::Bottom: granted = Rect{usable.x, usable.bottom() - t, usable.width, t}; usable.height -= t; break;
            case Edge::Left: granted = Rect{usable.x, usable.y, t, usable.height}; usable.x += t; usable.width -= t; break;
            case Edge::Right: granted = Rect{usable.right() - t, usable.y, t, usable.height}; usable.width -= t; break;
        }
        if (!(granted == r.rect)) {
            r.rect = granted;
            events.push(ReservationChanged{r.id, out.id, granted});
        }
    }

    // Collect this output's layer surfaces, top layer first for exclusivity.
    std::vector<LayerRec*> mine;
    for (auto& [k, l] : layers)
        if (l->layer->output == out.output && l->layer->initialized) mine.push_back(l.get());
    std::stable_sort(mine.begin(), mine.end(), [](LayerRec* a, LayerRec* b) {
        return a->layer->current.layer > b->layer->current.layer;
    });

    std::vector<std::pair<LayerRec*, Rect>> placed;
    // Pass 1: exclusive zones, in stacking order.
    for (LayerRec* l : mine) {
        const auto& st = l->layer->current;
        if (st.exclusive_zone <= 0 || !l->mapped) continue;
        Rect box = place(usable, st);
        Rect strip = apply_exclusive(usable, st.anchor, st.exclusive_zone, st);
        placed.emplace_back(l, box);
        if (l->reservation == kNoReservation) l->reservation = next_reservation++;
        if (!(strip == l->strip)) {
            l->strip = strip;
            events.push(ReservationChanged{l->reservation, out.id, strip});
        }
    }
    // Pass 2: everything else inside what is left (or the full output for -1).
    for (LayerRec* l : mine) {
        const auto& st = l->layer->current;
        if (st.exclusive_zone > 0 && l->mapped) continue;
        if (l->reservation != kNoReservation) {
            events.push(ReservationChanged{l->reservation, out.id, Rect{}});
            l->reservation = kNoReservation;
            l->strip = Rect{};
        }
        Rect bounds = st.exclusive_zone == -1 ? full : usable;
        placed.emplace_back(l, place(bounds, st));
    }

    for (auto& [l, box] : placed) {
        bool resize = int32_t(l->layer->current.actual_width) != box.width ||
                      int32_t(l->layer->current.actual_height) != box.height || !l->layer->configured;
        if (resize && box.width >= 0 && box.height >= 0)
            wlr_layer_surface_v1_configure(l->layer, uint32_t(box.width), uint32_t(box.height));
        l->rect = box;
        LayerSurfaceInfo info = layer_info(*l);
        if (l->announced && !(info == l->last)) server_events.push(LayerSurfaceChanged{info});
        l->last = info;
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.layers.find(l->id);
        if (it != mirror.layers.end()) it->second.info = info;
    }

    if (!(usable == out.work_area)) {
        out.work_area = usable;
        mark_outputs_dirty();
    }
}

void Server::arrange_all() {
    for (auto& [id, out] : outputs)
        if (out->output->enabled) arrange_output(*out);
}

void Server::on_new_layer_surface(wlr_layer_surface_v1* layer) {
    if (!layer->output) {
        wlr_output* o = output_at(mirror.cursor_x, mirror.cursor_y);
        if (!o && !outputs.empty()) o = outputs.begin()->second->output;
        if (!o) {
            wlr_layer_surface_v1_destroy(layer);
            return;
        }
        layer->output = o;
    }
    auto rec_ptr = std::make_unique<LayerRec>();
    LayerRec* l = rec_ptr.get();
    l->srv = this;
    l->layer = layer;
    l->id = next_layer++;
    layers[layer] = std::move(rec_ptr);
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.layers[l->id] = LayerMirror{};
    }

    l->commit.connect(&layer->surface->events.commit, [this, l](void*) {
        OutputRec* out = output_rec(l->layer->output);
        if (!out) return;
        if (l->layer->initial_commit || l->layer->current.committed) arrange_output(*out);
    });
    l->map.connect(&layer->surface->events.map, [this, l](void*) {
        l->mapped = true;
        if (OutputRec* out = output_rec(l->layer->output)) arrange_output(*out);
        l->last = layer_info(*l);
        l->announced = true;
        {
            std::lock_guard<std::mutex> lock(mirror.m);
            mirror.layers[l->id].info = l->last;
        }
        server_events.push(LayerSurfaceAdded{l->last});
        mark_tree_dirty(l->layer->surface);
        if (l->layer->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
            l->layer->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
            focus_surface(l->layer->surface);
    });
    l->unmap.connect(&layer->surface->events.unmap, [this, l](void*) {
        l->mapped = false;
        l->announced = false;
        if (l->reservation != kNoReservation) {
            OutputRec* o = output_rec(l->layer->output);
            events.push(ReservationChanged{l->reservation, o ? o->id : kNoMonitor, Rect{}});
        }
        l->reservation = kNoReservation;
        l->strip = Rect{};
        server_events.push(LayerSurfaceRemoved{l->id});
        if (focused_layer == l->layer->surface) {
            focused_layer = nullptr;
            WindowId w = focused_window;
            focused_window = kNoWindow;
            focus_window(w);
        }
        if (OutputRec* out = output_rec(l->layer->output)) arrange_output(*out);
    });
    l->destroy.connect(&layer->events.destroy, [this, layer, l](void*) {
        LayerSurfaceId id = l->id;
        wlr_output* o = l->layer->output;
        {
            std::lock_guard<std::mutex> lock(mirror.m);
            mirror.layers.erase(id);
        }
        layers.erase(layer);
        if (OutputRec* out = output_rec(o)) arrange_output(*out);
        mark_all_trees_dirty();
    });
}

}  // namespace brocompositor::wl
