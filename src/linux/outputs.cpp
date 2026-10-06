// Outputs: discovery, modesetting, the layout, frame pacing and the present
// path (host-rendered image -> wlr_output commit -> presentation feedback).
#include "linux/drm_util.h"
#include "linux/server_impl.h"

#include <drm_fourcc.h>
#include <unistd.h>

#include <cmath>
#include <ctime>

namespace brocompositor::wl {

wlr_buffer* slot_buffer(OutputImageSlot* s);
const SharedImage& slot_desc(OutputImageSlot* s);

namespace {

int64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Pixel size the output's buffers must have after `state` is applied.
Size target_size(wlr_output* o, const wlr_output_state* st) {
    if (st->committed & WLR_OUTPUT_STATE_MODE) {
        if (st->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) return Size{st->custom_mode.width, st->custom_mode.height};
        if (st->mode) return Size{st->mode->width, st->mode->height};
    }
    return Size{o->width, o->height};
}

}  // namespace

OutputRec* Server::output_rec(MonitorId id) {
    auto it = outputs.find(id);
    return it == outputs.end() ? nullptr : it->second.get();
}

OutputRec* Server::output_rec(wlr_output* output) {
    if (!output) return nullptr;
    for (auto& [id, o] : outputs)
        if (o->output == output) return o.get();
    return nullptr;
}

wlr_output* Server::output_at(double x, double y) { return wlr_output_layout_output_at(layout, x, y); }

bool Server::apply_output_state(OutputRec& out, wlr_output_state* st) {
    wlr_output* o = out.output;
    bool enabling = (st->committed & WLR_OUTPUT_STATE_ENABLED) ? st->enabled : o->enabled;
    if (!enabling) {
        bool ok = wlr_output_commit_state(o, st);
        if (ok) free_output_images(out);
        return ok;
    }
    Size sz = target_size(o, st);
    if (sz.empty()) return false;
    // Lighting up an output or changing its mode needs a buffer of the new size.
    bool has_buffer = (st->committed & WLR_OUTPUT_STATE_BUFFER) != 0;
    bool needs_buffer = !has_buffer && (!o->enabled || (st->committed & WLR_OUTPUT_STATE_MODE));
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (needs_buffer) {
            if (!ensure_output_images(out, sz.width, sz.height)) return false;
            // Show a free image (the host's next frame replaces it).
            OutputImageSlot* free_slot = nullptr;
            {
                std::lock_guard<std::mutex> lock(mirror.m);
                auto& imgs = mirror.outputs[out.id].images;
                for (auto& img : imgs)
                    if (img.state == SlotState::Free) {
                        free_slot = slot(out, img.desc.id);
                        break;
                    }
            }
            if (!free_slot) return false;
            wlr_output_state_set_buffer(st, slot_buffer(free_slot));
            set_image_state(out.id, slot_desc(free_slot).id, SlotState::Scanout);
            if (wlr_output_test_state(o, st)) {
                if (wlr_output_commit_state(o, st)) return true;
            }
            set_image_state(out.id, slot_desc(free_slot).id, SlotState::Free);
            // A nested parent may refuse our dmabufs: retry with shm images.
            if (attempt == 0 && !is_drm_output(o) && !out.use_shm && !out.slots.empty() &&
                slot_desc(out.slots[0]).type == ImageHandleType::DmaBuf) {
                out.use_shm = true;
                free_output_images(out);
                st->committed &= ~uint32_t(WLR_OUTPUT_STATE_BUFFER);
                wlr_buffer_unlock(st->buffer);
                st->buffer = nullptr;
                continue;
            }
            return false;
        }
        return wlr_output_commit_state(o, st);
    }
    return false;
}

void Server::on_new_output(wlr_output* o) {
    auto rec_ptr = std::make_unique<OutputRec>();
    OutputRec* out = rec_ptr.get();
    out->srv = this;
    out->output = o;
    out->id = next_monitor++;
    outputs[out->id] = std::move(rec_ptr);
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.outputs[out->id] = OutputMirror{};
    }

    out->frame.connect(&o->events.frame, [this, out](void*) { server_events.push(OutputFrame{out->id}); });
    out->present.connect(&o->events.present, [this, out](void* data) {
        auto* e = static_cast<wlr_output_event_present*>(data);
        OutputPresented p;
        p.output = out->id;
        p.commit_seq = e->commit_seq;
        p.presented = e->presented;
        p.timestamp_ns = e->when ? int64_t(e->when->tv_sec) * 1000000000 + e->when->tv_nsec : now_ns();
        p.refresh_ns = e->refresh > 0 ? uint32_t(e->refresh) : 0;
        auto it = out->inflight.find(e->commit_seq);
        if (it != out->inflight.end()) {
            p.image_id = it->second;
            out->inflight.erase(out->inflight.begin(), std::next(it));
        }
        server_events.push(p);
    });
    out->commit.connect(&o->events.commit, [this](void* data) {
        auto* e = static_cast<wlr_output_event_commit*>(data);
        constexpr uint32_t kLayoutFields = WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED |
                                           WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM;
        if (e->state->committed & kLayoutFields) {
            arrange_all();
            mark_outputs_dirty();
            update_all_window_outputs();
        }
    });
    out->request_state.connect(&o->events.request_state, [this, out](void* data) {
        auto* e = static_cast<wlr_output_event_request_state*>(data);
        wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_copy(&st, e->state);
        apply_output_state(*out, &st);
        wlr_output_state_finish(&st);
    });
    out->destroy.connect(&o->events.destroy, [this, out](void*) {
        MonitorId id = out->id;
        fail_output_waiters(*out);
        gamma_output_gone(*out);
        free_output_images(*out);
        for (auto& [k, l] : layers)
            if (l->layer->output == out->output) l->layer->output = nullptr;
        for (auto& [k, t] : toplevels) t->entered.erase(out->output);
#ifdef BC_HAVE_XWAYLAND
        for (auto& [k, x] : xsurfaces) x->entered.erase(out->output);
#endif
        lock_waiting.erase(id);
        {
            std::lock_guard<std::mutex> lock(mirror.m);
            mirror.outputs.erase(id);
        }
        // Layer surfaces on a vanished output are closed (protocol rule).
        std::vector<wlr_layer_surface_v1*> orphans;
        for (auto& [k, l] : layers)
            if (!l->layer->output) orphans.push_back(k);
        outputs.erase(id);
        for (auto* l : orphans) wlr_layer_surface_v1_destroy(l);
        mark_outputs_dirty();
    });

    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    if (wlr_output_mode* m = wlr_output_preferred_mode(o)) {
        wlr_output_state_set_mode(&st, m);
    } else {
        // Headless and nested outputs have no modes: use the size asked for
        // (add_output() sets it; the nested backends default to 1280x720).
        Size s = config.initial_output_size;
        wlr_output_state_set_custom_mode(&st, s.width, s.height,
                                         wlr_output_is_headless(o) ? config.headless_refresh_mhz : 0);
    }
    if (config.initial_scale > 0) wlr_output_state_set_scale(&st, config.initial_scale);
    if (!apply_output_state(*out, &st)) wlr_log(WLR_ERROR, "brocompositor: cannot enable output %s", o->name);
    wlr_output_state_finish(&st);

    wlr_output_layout_add_auto(layout, o);
    out->work_area = Rect{};
    arrange_output(*out);
    mark_outputs_dirty();
}

MonitorId Server::add_output(Size size) {
    wlr_output* o = nullptr;
    Size saved = config.initial_output_size;
    config.initial_output_size = size;
    if (wlr_backend_is_headless(backend))
        o = wlr_headless_add_output(backend, uint32_t(size.width), uint32_t(size.height));
    else if (wlr_backend_is_wl(backend))
        o = wlr_wl_output_create(backend);
    else if (wlr_backend_is_x11(backend))
        o = wlr_x11_output_create(backend);
    config.initial_output_size = saved;
    OutputRec* r = output_rec(o);
    return r ? r->id : kNoMonitor;
}

bool Server::configure_output(MonitorId id, const OutputConfig& c) {
    OutputRec* out = output_rec(id);
    if (!out) return false;
    wlr_output* o = out->output;
    wlr_output_state st;
    wlr_output_state_init(&st);
    if (c.enabled) wlr_output_state_set_enabled(&st, *c.enabled);
    if (c.mode) {
        wlr_output_mode* best = nullptr;
        wlr_output_mode* m;
        wl_list_for_each(m, &o->modes, link) {
            if (m->width != c.mode->width || m->height != c.mode->height) continue;
            if (c.refresh_mhz && m->refresh != *c.refresh_mhz) {
                if (!best) best = m;
                continue;
            }
            best = m;
            break;
        }
        if (best) wlr_output_state_set_mode(&st, best);
        else wlr_output_state_set_custom_mode(&st, c.mode->width, c.mode->height, c.refresh_mhz.value_or(0));
    }
    if (c.scale) wlr_output_state_set_scale(&st, *c.scale);
    if (c.transform) wlr_output_state_set_transform(&st, wl_output_transform(*c.transform));
    bool ok = st.committed ? apply_output_state(*out, &st) : true;
    wlr_output_state_finish(&st);
    if (ok && c.position) {
        wlr_output_layout_add(layout, o, c.position->x, c.position->y);
        arrange_all();
        update_all_window_outputs();
        mark_outputs_dirty();
    }
    return ok;
}

OutputInfo Server::output_info(OutputRec& out) {
    wlr_output* o = out.output;
    OutputInfo i;
    i.id = out.id;
    i.name = o->name ? o->name : "";
    i.description = o->description ? o->description : "";
    i.make = o->make ? o->make : "";
    i.model = o->model ? o->model : "";
    i.enabled = o->enabled;
    i.pixel_size = Size{o->width, o->height};
    i.refresh_mhz = o->refresh;
    i.scale = o->scale;
    i.transform = uint32_t(o->transform);
    wlr_box b{};
    wlr_output_layout_get_box(layout, o, &b);
    i.layout = Rect{b.x, b.y, b.width, b.height};
    i.work_area = out.work_area.empty() ? i.layout : out.work_area;
    wlr_output_mode* m;
    wl_list_for_each(m, &o->modes, link) i.modes.push_back(OutputMode{Size{m->width, m->height}, m->refresh, m->preferred});
    i.images_generation = out.images_generation;
    return i;
}

void Server::mark_outputs_dirty() {
    outputs_dirty = true;
    if (!outputs_idle)
        outputs_idle = wl_event_loop_add_idle(
            loop, [](void* data) { static_cast<Server*>(data)->publish_outputs(); }, this);
}

void Server::publish_outputs() {
    outputs_idle = nullptr;
    if (!outputs_dirty) return;
    outputs_dirty = false;
    std::vector<OutputInfo> infos;
    std::vector<MonitorSnapshot> mons;
    bool first = true;
    for (auto& [id, out] : outputs) {
        OutputInfo i = output_info(*out);
        infos.push_back(i);
        if (!i.enabled || i.layout.empty()) continue;
        MonitorSnapshot m;
        m.id = id;
        m.name = i.name;
        m.bounds = i.layout;
        m.work_area = i.work_area;
        m.dpi = uint32_t(std::lround(96.0 * i.scale));
        m.primary = first;
        first = false;
        mons.push_back(m);
    }
    bool mons_changed;
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        for (auto& i : infos) mirror.outputs[i.id].info = i;
        mons_changed = mirror.monitors != mons;
        mirror.monitors = mons;
    }
    if (mons_changed) events.push(MonitorsChanged{mons});
    server_events.push(OutputsChanged{infos});
    update_output_manager();
    update_xwayland_workareas();
    configure_lock_surfaces();
}

void Server::present(MonitorId id, PresentRequest req) {
    int sync = fd_of(req.render_done);
    auto fail = [&] {
        if (sync >= 0) ::close(sync);
        set_image_state(id, req.image_id, SlotState::Free);
        server_events.push(OutputPresentFailed{id, req.image_id});
    };
    OutputRec* out = output_rec(id);
    OutputImageSlot* s = out ? slot(*out, req.image_id) : nullptr;
    if (!out || !s || !out->output->enabled) return fail();
    const SharedImage& d = slot_desc(s);
    if (sync >= 0) {
        // KMS and nested parents honour the dmabuf's implicit fence; for shm
        // (or kernels without sync_file import) wait on the CPU.
        bool attached = d.type == ImageHandleType::DmaBuf && import_sync_file(fd_of(d.planes[0].handle), sync);
        if (!attached) wait_sync_file(sync, 1000);
        ::close(sync);
        sync = -1;
    }
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_buffer(&st, slot_buffer(s));
    pixman_region32_t damage;
    pixman_region32_init(&damage);
    if (req.damage.empty()) {
        pixman_region32_union_rect(&damage, &damage, 0, 0, uint32_t(d.width), uint32_t(d.height));
    } else {
        for (const Rect& r : req.damage)
            pixman_region32_union_rect(&damage, &damage, r.x, r.y, uint32_t(std::max(0, r.width)),
                                       uint32_t(std::max(0, r.height)));
    }
    wlr_output_state_set_damage(&st, &damage);
    pixman_region32_fini(&damage);
    apply_gamma(*out, &st);

    // While locked only lock surfaces count as drawn: nothing else gets frame
    // callbacks or presentation feedback (a hidden client stays throttled).
    // A frame that still drew some other surface was composed before the host
    // saw the lock, so it does not count towards the lock being in effect.
    std::vector<wlr_surface*> drawn;
    bool lock_clean = true;
    for (SurfaceId sid : req.surfaces)
        if (wlr_surface* ws = surface_by_id(sid)) {
            if (!locked() || lock_allows(ws))
                drawn.push_back(ws);
            else
                lock_clean = false;
        }
    for (wlr_surface* ws : drawn) wlr_presentation_surface_textured_on_output(ws, out->output);

    set_image_state(id, req.image_id, SlotState::Scanout);
    bool ok = wlr_output_commit_state(out->output, &st);
    wlr_output_state_finish(&st);
    if (!ok) return fail();
    out->inflight[out->output->commit_seq] = req.image_id;
    set_front_image(*out, req.image_id);
    int64_t t = now_ns();
    for (wlr_surface* ws : drawn) send_frame_done(ws, t);
    if (lock_clean) lock_output_presented(id);
    notify_presented(*out, s, req.damage, t);
}

ReservationId Server::reserve_edge(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted) {
    OutputRec* out = output_rec(monitor);
    if (!out || thickness <= 0) return kNoReservation;
    HostReservation r;
    r.id = next_reservation++;
    r.monitor = monitor;
    r.edge = edge;
    r.thickness = thickness;
    host_reservations[r.id] = r;
    arrange_output(*out);
    if (granted) *granted = host_reservations[r.id].rect;
    return r.id;
}

bool Server::release_edge(ReservationId id) {
    auto it = host_reservations.find(id);
    if (it == host_reservations.end()) return false;
    MonitorId m = it->second.monitor;
    host_reservations.erase(it);
    if (OutputRec* out = output_rec(m)) arrange_output(*out);
    return true;
}

}  // namespace brocompositor::wl
