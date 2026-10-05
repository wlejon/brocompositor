// wlr-screencopy-unstable-v1 (v3), the protocol grim, wf-recorder and most
// wlroots-era tools speak. Implemented here because wlroots 0.18's version
// needs a wlr_renderer; the copy itself is capture.cpp's (CPU or host).
//
//   * copy: the output's current image (or the first one presented);
//   * copy_with_damage: waits until the output changed since this client's
//     last copy, then reports the damage accumulated since.
#include "linux/capture_util.h"

#include "wlr-screencopy-unstable-v1-protocol.h"

#include <algorithm>
#include <cmath>

namespace brocompositor::wl {

namespace {

constexpr uint32_t kVersion = 3;

// One bound zwlr_screencopy_manager_v1: damage per output since its last copy.
struct ScClient {
    Server* srv = nullptr;
    std::map<MonitorId, std::shared_ptr<DamageAccum>> damage;
};
using ClientPtr = std::shared_ptr<ScClient>;

struct ScFrame {
    Server* srv = nullptr;
    wl_resource* resource = nullptr;  // null once the client destroyed it
    ClientPtr client;
    MonitorId output = kNoMonitor;
    Rect region;  // output pixels
    uint32_t format = 0;
    bool cursor = false;
    bool used = false;
};
using FramePtr = std::shared_ptr<ScFrame>;

FramePtr& frame_of(wl_resource* r) { return *static_cast<FramePtr*>(wl_resource_get_user_data(r)); }
ClientPtr& client_of(wl_resource* r) { return *static_cast<ClientPtr*>(wl_resource_get_user_data(r)); }

void send_failed(ScFrame& f) {
    if (f.resource) zwlr_screencopy_frame_v1_send_failed(f.resource);
}

void frame_finish(const FramePtr& f, CaptureJob& job, bool ok) {
    if (!f->resource) return;
    if (!ok) return send_failed(*f);
    zwlr_screencopy_frame_v1_send_flags(f->resource, 0);
    if (wl_resource_get_version(f->resource) >= 2)
        for (const Rect& r : job.damage) {
            // Damage is reported relative to the captured region.
            int x0 = std::max(r.x, f->region.x) - f->region.x, y0 = std::max(r.y, f->region.y) - f->region.y;
            int x1 = std::min(r.x + r.width, f->region.x + f->region.width) - f->region.x;
            int y1 = std::min(r.y + r.height, f->region.y + f->region.height) - f->region.y;
            if (x1 > x0 && y1 > y0)
                zwlr_screencopy_frame_v1_send_damage(f->resource, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0),
                                                     uint32_t(y1 - y0));
        }
    uint32_t hi, lo, ns;
    split_time(job.when_ns ? job.when_ns : monotonic_ns(), &hi, &lo, &ns);
    zwlr_screencopy_frame_v1_send_ready(f->resource, hi, lo, ns);
}

void frame_copy(wl_resource* resource, wl_resource* buffer_resource, bool with_damage) {
    FramePtr f = frame_of(resource);
    Server* s = f->srv;
    if (f->used) {
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED, "frame already used");
        return;
    }
    f->used = true;
    OutputRec* out = s->output_rec(f->output);
    if (!out || !out->output->enabled) return send_failed(*f);
    wlr_buffer* buffer = wlr_buffer_try_from_resource(buffer_resource);
    if (!capture_buffer_ok(buffer, f->format, f->region.width, f->region.height)) {
        if (buffer) wlr_buffer_unlock(buffer);
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer");
        return;
    }
    auto job = std::make_unique<CaptureJob>();
    job->output = f->output;
    job->region = f->region;
    job->cursor = f->cursor;
    job->buffer = buffer;
    job->finish = [f](CaptureJob& j, bool ok) { frame_finish(f, j, ok); };

    std::shared_ptr<DamageAccum>& acc = f->client->damage[f->output];
    if (!acc) {
        acc = std::make_shared<DamageAccum>();
        track_output_damage(*out, acc);
    }
    OutputCaptureInfo info = output_capture_info(*out);
    OutputImageSlot* front = front_slot(*s, *out);
    // Copy now when there is something to report; otherwise at the next present.
    if (front && (!with_damage || !acc->empty())) {
        job->damage = acc->take(info.width, info.height);
        job->when_ns = monotonic_ns();
        s->run_output_capture(std::move(job), front);
        return;
    }
    auto holder = std::make_shared<std::unique_ptr<CaptureJob>>(std::move(job));
    std::weak_ptr<DamageAccum> wacc = acc;
    out->present_waiters.push_back(
        [s, holder, wacc, info, with_damage](OutputImageSlot* image, const std::vector<Rect>& damage, int64_t when) {
            auto acc = wacc.lock();
            if (with_damage && image && (acc ? acc->empty() : damage_is_none(damage))) return false;  // nothing new yet
            std::unique_ptr<CaptureJob> j = std::move(*holder);
            if (auto a = acc)
                j->damage = a->take(info.width, info.height);  // the tracker ran first and added this present
            else
                j->damage = damage.empty() ? std::vector<Rect>{Rect{0, 0, info.width, info.height}} : damage;
            j->when_ns = when;
            s->run_output_capture(std::move(j), image);
            return true;
        });
    // copy_with_damage waits for real damage; only a missing image forces a frame.
    if (!front) wlr_output_schedule_frame(out->output);
}

const struct zwlr_screencopy_frame_v1_interface kFrameImpl = {
    [](wl_client*, wl_resource* r, wl_resource* buffer) { frame_copy(r, buffer, false); },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    [](wl_client*, wl_resource* r, wl_resource* buffer) { frame_copy(r, buffer, true); },
};

void frame_destroy(wl_resource* r) {
    auto* p = static_cast<FramePtr*>(wl_resource_get_user_data(r));
    (*p)->resource = nullptr;
    delete p;
}

void capture(wl_client* client, wl_resource* manager, uint32_t id, int32_t overlay_cursor, wl_resource* output,
             const wlr_box* logical_region) {
    ClientPtr& c = client_of(manager);
    Server* s = c->srv;
    wl_resource* fr = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
                                         wl_resource_get_version(manager), id);
    if (!fr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto f = std::make_shared<ScFrame>();
    f->srv = s;
    f->resource = fr;
    f->client = c;
    f->cursor = overlay_cursor != 0;
    wl_resource_set_implementation(fr, &kFrameImpl, new FramePtr(f), frame_destroy);

    wlr_output* wo = wlr_output_from_resource(output);
    OutputRec* out = wo ? s->output_rec(wo) : nullptr;
    if (!out || !wo->enabled) {
        f->used = true;
        zwlr_screencopy_frame_v1_send_failed(fr);
        return;
    }
    f->output = out->id;
    OutputCaptureInfo info = output_capture_info(*out);
    f->format = info.drm_format;
    f->region = Rect{0, 0, info.width, info.height};
    if (logical_region) {
        // Logical (layout-sized) coordinates to output pixels, clipped.
        double sx = wo->scale > 0 ? wo->scale : 1.0;
        int x0 = int(std::floor(logical_region->x * sx)), y0 = int(std::floor(logical_region->y * sx));
        int x1 = int(std::ceil((logical_region->x + logical_region->width) * sx));
        int y1 = int(std::ceil((logical_region->y + logical_region->height) * sx));
        x0 = std::clamp(x0, 0, info.width);
        y0 = std::clamp(y0, 0, info.height);
        x1 = std::clamp(x1, 0, info.width);
        y1 = std::clamp(y1, 0, info.height);
        if (x1 <= x0 || y1 <= y0) {
            f->used = true;
            zwlr_screencopy_frame_v1_send_failed(fr);
            return;
        }
        f->region = Rect{x0, y0, x1 - x0, y1 - y0};
    }
    uint32_t bpp = 4;
    zwlr_screencopy_frame_v1_send_buffer(fr, drm_to_shm(f->format), uint32_t(f->region.width),
                                         uint32_t(f->region.height), uint32_t(f->region.width) * bpp);
    if (wl_resource_get_version(fr) >= 3) {
        zwlr_screencopy_frame_v1_send_linux_dmabuf(fr, f->format, uint32_t(f->region.width),
                                                   uint32_t(f->region.height));
        zwlr_screencopy_frame_v1_send_buffer_done(fr);
    }
}

const struct zwlr_screencopy_manager_v1_interface kManagerImpl = {
    [](wl_client* c, wl_resource* m, uint32_t id, int32_t cursor, wl_resource* output) {
        capture(c, m, id, cursor, output, nullptr);
    },
    [](wl_client* c, wl_resource* m, uint32_t id, int32_t cursor, wl_resource* output, int32_t x, int32_t y,
       int32_t w, int32_t h) {
        if (w <= 0 || h <= 0) {
            wl_resource_post_error(m, WL_DISPLAY_ERROR_INVALID_METHOD, "invalid region");
            return;
        }
        wlr_box b{x, y, w, h};
        capture(c, m, id, cursor, output, &b);
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

void bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface,
                                        int(std::min(version, kVersion)), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    auto c = std::make_shared<ScClient>();
    c->srv = static_cast<Server*>(data);
    wl_resource_set_implementation(r, &kManagerImpl, new ClientPtr(c), [](wl_resource* res) {
        delete static_cast<ClientPtr*>(wl_resource_get_user_data(res));
    });
}

}  // namespace

void Server::init_screencopy() {
    screencopy_global = wl_global_create(display, &zwlr_screencopy_manager_v1_interface, int(kVersion), this, bind);
}

}  // namespace brocompositor::wl
