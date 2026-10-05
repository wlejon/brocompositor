// ext-image-copy-capture-v1 with ext-image-capture-source-v1: the
// standardized successor of wlr-screencopy (xdg-desktop-portal-wlr, OBS,
// newer grim). Sources are outputs (ext_output_image_capture_source_manager)
// and windows, named by ext-foreign-toplevel-list handles
// (ext_foreign_toplevel_image_capture_source_manager). Output copies are
// capture.cpp's (CPU or host); window copies are always the host's render
// of the window tree. Cursor sessions are accepted but carry no cursor
// image: the host draws the cursor, so their capture sessions are stopped.
#include "linux/capture_util.h"
#include "linux/drm_util.h"

#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"

#include <drm_fourcc.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace brocompositor::wl {

namespace {

// ---------------------------------------------------------------- sources

struct SourceSpec {
    MonitorId output = kNoMonitor;
    WindowId window = kNoWindow;  // kNoWindow with output == kNoMonitor: a dead source
};

SourceSpec* source_of(wl_resource* r) { return static_cast<SourceSpec*>(wl_resource_get_user_data(r)); }

const struct ext_image_capture_source_v1_interface kSourceImpl = {
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

void create_source(wl_client* client, wl_resource* manager, uint32_t id, SourceSpec spec) {
    wl_resource* r = wl_resource_create(client, &ext_image_capture_source_v1_interface,
                                        wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kSourceImpl, new SourceSpec(spec),
                                   [](wl_resource* res) { delete source_of(res); });
}

Server* server_of(wl_resource* r) { return static_cast<Server*>(wl_resource_get_user_data(r)); }

const struct ext_output_image_capture_source_manager_v1_interface kOutputSourceImpl = {
    [](wl_client* c, wl_resource* m, uint32_t id, wl_resource* output) {
        SourceSpec spec;
        wlr_output* wo = wlr_output_from_resource(output);
        if (OutputRec* o = wo ? server_of(m)->output_rec(wo) : nullptr) spec.output = o->id;
        create_source(c, m, id, spec);
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

const struct ext_foreign_toplevel_image_capture_source_manager_v1_interface kToplevelSourceImpl = {
    [](wl_client* c, wl_resource* m, uint32_t id, wl_resource* handle) {
        SourceSpec spec;
        spec.window = server_of(m)->window_of_ext_handle(handle);
        create_source(c, m, id, spec);
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

// ---------------------------------------------------------------- sessions / frames

struct Session {
    Server* srv = nullptr;
    wl_resource* resource = nullptr;  // null once destroyed
    SourceSpec src;
    bool cursors = false;
    bool stopped = false;
    bool first = true;  // no frame captured yet
    int width = 0, height = 0;
    uint32_t format = 0;
    float scale = 1.0f;  // window source
    std::shared_ptr<DamageAccum> damage;  // output source
    wl_resource* frame = nullptr;         // the one live frame
};
using SessionPtr = std::shared_ptr<Session>;

struct CopyFrame {
    SessionPtr session;
    wl_resource* resource = nullptr;  // null once destroyed
    wlr_buffer* buffer = nullptr;     // attached (locked)
    std::vector<Rect> buffer_damage;
    bool captured = false;
};
using CopyFramePtr = std::shared_ptr<CopyFrame>;

SessionPtr& session_of(wl_resource* r) { return *static_cast<SessionPtr*>(wl_resource_get_user_data(r)); }
CopyFramePtr& copy_frame_of(wl_resource* r) { return *static_cast<CopyFramePtr*>(wl_resource_get_user_data(r)); }

void stop_session(Session& s) {
    if (s.stopped) return;
    s.stopped = true;
    if (s.resource) ext_image_copy_capture_session_v1_send_stopped(s.resource);
}

// Buffer constraints of the source as it is now. False when the source is gone.
bool current_constraints(Session& s, int* w, int* h, uint32_t* format, float* scale) {
    Server* srv = s.srv;
    if (s.src.output != kNoMonitor) {
        OutputRec* out = srv->output_rec(s.src.output);
        if (!out || !out->output->enabled) return false;
        OutputCaptureInfo i = output_capture_info(*out);
        *w = i.width;
        *h = i.height;
        *format = i.drm_format;
        *scale = 1.0f;
        return true;
    }
    if (s.src.window == kNoWindow || srv->locked()) return false;
    WindowRef ref = srv->window_ref(s.src.window);
    if (!ref) return false;
    const WindowSnapshot& snap = ref.xdg ? ref.xdg->snap : ref.x->snap;
    float sc = snap.dpi > 0 ? float(snap.dpi) / 96.0f : 1.0f;
    *scale = sc;
    *w = std::max(1, int(std::ceil(snap.frame.width * sc)));
    *h = std::max(1, int(std::ceil(snap.frame.height * sc)));
    *format = DRM_FORMAT_ARGB8888;
    return true;
}

void send_constraints(Session& s) {
    if (!current_constraints(s, &s.width, &s.height, &s.format, &s.scale)) return stop_session(s);
    wl_resource* r = s.resource;
    ext_image_copy_capture_session_v1_send_buffer_size(r, uint32_t(s.width), uint32_t(s.height));
    ext_image_copy_capture_session_v1_send_shm_format(r, drm_to_shm(s.format));
    if (s.srv->render_fd >= 0) {
        uint64_t dev = dev_of_fd(s.srv->render_fd);
        dev_t d = dev_t(dev);
        wl_array a;
        wl_array_init(&a);
        if (void* p = wl_array_add(&a, sizeof d)) std::memcpy(p, &d, sizeof d);
        ext_image_copy_capture_session_v1_send_dmabuf_device(r, &a);
        wl_array_release(&a);
        wl_array mods;
        wl_array_init(&mods);
        uint64_t linear = DRM_FORMAT_MOD_LINEAR;
        if (void* p = wl_array_add(&mods, sizeof linear)) std::memcpy(p, &linear, sizeof linear);
        ext_image_copy_capture_session_v1_send_dmabuf_format(r, s.format, &mods);
        wl_array_release(&mods);
    }
    ext_image_copy_capture_session_v1_send_done(r);
}

void frame_failed(CopyFrame& f, uint32_t reason) {
    if (f.resource) ext_image_copy_capture_frame_v1_send_failed(f.resource, reason);
}

void frame_ready(const CopyFramePtr& f, CaptureJob& job, bool ok) {
    if (!f->resource) return;
    if (!ok) {
        Session& s = *f->session;
        int w, h;
        uint32_t fmt;
        float sc;
        if (!s.stopped && !current_constraints(s, &w, &h, &fmt, &sc)) stop_session(s);
        return frame_failed(*f, s.stopped ? EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED
                                          : EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
    }
    ext_image_copy_capture_frame_v1_send_transform(f->resource, WL_OUTPUT_TRANSFORM_NORMAL);
    for (const Rect& r : job.damage)
        ext_image_copy_capture_frame_v1_send_damage(f->resource, r.x, r.y, r.width, r.height);
    // Buffer damage was copied too, so it changed: report what the source
    // damage does not already cover.
    for (const Rect& r : f->buffer_damage) {
        bool covered = false;
        for (const Rect& d : job.damage)
            covered = covered || (r.x >= d.x && r.y >= d.y && r.x + r.width <= d.x + d.width &&
                                  r.y + r.height <= d.y + d.height);
        if (!covered) ext_image_copy_capture_frame_v1_send_damage(f->resource, r.x, r.y, r.width, r.height);
    }
    uint32_t hi, lo, ns;
    split_time(job.when_ns ? job.when_ns : monotonic_ns(), &hi, &lo, &ns);
    ext_image_copy_capture_frame_v1_send_presentation_time(f->resource, hi, lo, ns);
    ext_image_copy_capture_frame_v1_send_ready(f->resource);
}

void frame_capture(wl_resource* resource) {
    CopyFramePtr f = copy_frame_of(resource);
    if (f->captured) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED, "already captured");
        return;
    }
    if (!f->buffer) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER, "no buffer attached");
        return;
    }
    f->captured = true;
    Session& s = *f->session;
    Server* srv = s.srv;
    if (s.stopped || !s.resource) return frame_failed(*f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    int w, h;
    uint32_t fmt;
    float scale;
    if (!current_constraints(s, &w, &h, &fmt, &scale)) {
        stop_session(s);
        return frame_failed(*f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    }
    if (w != s.width || h != s.height || fmt != s.format) {
        // The source changed size: new constraints, then the client retries.
        send_constraints(s);
        return frame_failed(*f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
    }
    if (!capture_buffer_ok(f->buffer, s.format, s.width, s.height))
        return frame_failed(*f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);

    auto job = std::make_unique<CaptureJob>();
    job->buffer = wlr_buffer_lock(f->buffer);
    job->cursor = s.cursors;
    job->finish = [f](CaptureJob& j, bool ok) { frame_ready(f, j, ok); };
    bool first = s.first;
    s.first = false;

    if (s.src.window != kNoWindow) {
        job->window = s.src.window;
        job->scale = s.scale;
        job->damage = {Rect{0, 0, s.width, s.height}};
        if (first) {
            job->when_ns = monotonic_ns();
            return srv->run_window_capture(std::move(job));
        }
        // Later frames wait for the window to show something new.
        auto holder = std::make_shared<std::unique_ptr<CaptureJob>>(std::move(job));
        srv->window_commit_waiters.emplace_back(s.src.window, [srv, holder] {
            std::unique_ptr<CaptureJob> j = std::move(*holder);
            j->when_ns = monotonic_ns();
            srv->run_window_capture(std::move(j));
            return true;
        });
        return;
    }

    OutputRec* out = srv->output_rec(s.src.output);
    job->output = s.src.output;
    job->region = Rect{0, 0, s.width, s.height};
    if (!s.damage) {
        s.damage = std::make_shared<DamageAccum>();
        track_output_damage(*out, s.damage);
    }
    OutputImageSlot* front = front_slot(*srv, *out);
    if (front && !s.damage->empty()) {
        job->damage = s.damage->take(s.width, s.height);
        job->when_ns = monotonic_ns();
        return srv->run_output_capture(std::move(job), front);
    }
    auto holder = std::make_shared<std::unique_ptr<CaptureJob>>(std::move(job));
    std::weak_ptr<DamageAccum> wacc = s.damage;
    int sw = s.width, sh = s.height;
    out->present_waiters.push_back(
        [srv, holder, wacc, sw, sh](OutputImageSlot* image, const std::vector<Rect>& damage, int64_t when) {
            auto acc = wacc.lock();
            if (image && (acc ? acc->empty() : damage_is_none(damage))) return false;  // nothing new yet
            std::unique_ptr<CaptureJob> j = std::move(*holder);
            if (auto a = acc)
                j->damage = a->take(sw, sh);
            else
                j->damage = damage.empty() ? std::vector<Rect>{Rect{0, 0, sw, sh}} : damage;
            j->when_ns = when;
            srv->run_output_capture(std::move(j), image);
            return true;
        });
    // Without a presented image yet, ask for one; otherwise the frame waits
    // for real damage (a forced frame would report a repaint as a change).
    if (!front) wlr_output_schedule_frame(out->output);
}

const struct ext_image_copy_capture_frame_v1_interface kFrameImpl = {
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    [](wl_client*, wl_resource* r, wl_resource* buffer) {
        CopyFramePtr& f = copy_frame_of(r);
        if (f->captured) {
            wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED, "already captured");
            return;
        }
        if (f->buffer) wlr_buffer_unlock(f->buffer);
        f->buffer = wlr_buffer_try_from_resource(buffer);
    },
    [](wl_client*, wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
        CopyFramePtr& f = copy_frame_of(r);
        if (f->captured) {
            wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED, "already captured");
            return;
        }
        if (x < 0 || y < 0 || w <= 0 || h <= 0) {
            wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE, "invalid damage");
            return;
        }
        f->buffer_damage.push_back(Rect{x, y, w, h});
    },
    [](wl_client*, wl_resource* r) { frame_capture(r); },
};

void frame_destroy(wl_resource* r) {
    auto* p = static_cast<CopyFramePtr*>(wl_resource_get_user_data(r));
    CopyFrame& f = **p;
    f.resource = nullptr;
    if (f.session->frame == r) f.session->frame = nullptr;
    if (f.buffer) wlr_buffer_unlock(f.buffer);
    f.buffer = nullptr;
    delete p;
}

const struct ext_image_copy_capture_session_v1_interface kSessionImpl = {
    [](wl_client* client, wl_resource* r, uint32_t id) {
        SessionPtr& s = session_of(r);
        if (s->frame) {
            wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME, "frame exists");
            return;
        }
        wl_resource* fr = wl_resource_create(client, &ext_image_copy_capture_frame_v1_interface,
                                             wl_resource_get_version(r), id);
        if (!fr) {
            wl_client_post_no_memory(client);
            return;
        }
        auto f = std::make_shared<CopyFrame>();
        f->session = s;
        f->resource = fr;
        s->frame = fr;
        wl_resource_set_implementation(fr, &kFrameImpl, new CopyFramePtr(f), frame_destroy);
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

void session_destroy(wl_resource* r) {
    auto* p = static_cast<SessionPtr*>(wl_resource_get_user_data(r));
    (*p)->resource = nullptr;
    (*p)->stopped = true;
    (*p)->damage.reset();
    delete p;
}

wl_resource* create_session_resource(wl_client* client, wl_resource* parent, uint32_t id, Server* srv,
                                     SourceSpec src, bool cursors, bool cursor_session) {
    wl_resource* r = wl_resource_create(client, &ext_image_copy_capture_session_v1_interface,
                                        wl_resource_get_version(parent), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return nullptr;
    }
    auto s = std::make_shared<Session>();
    s->srv = srv;
    s->resource = r;
    s->src = src;
    s->cursors = cursors;
    wl_resource_set_implementation(r, &kSessionImpl, new SessionPtr(s), session_destroy);
    if (cursor_session)
        stop_session(*s);  // no cursor images: the host draws the cursor
    else
        send_constraints(*s);
    return r;
}

// Cursor sessions: valid objects that never report a cursor.
struct CursorSession {
    Server* srv = nullptr;
    bool has_session = false;
};

const struct ext_image_copy_capture_cursor_session_v1_interface kCursorImpl = {
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    [](wl_client* client, wl_resource* r, uint32_t id) {
        auto* cs = static_cast<CursorSession*>(wl_resource_get_user_data(r));
        if (cs->has_session) {
            wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
                                   "duplicate session");
            return;
        }
        cs->has_session = true;
        create_session_resource(client, r, id, cs->srv, SourceSpec{}, false, true);
    },
};

const struct ext_image_copy_capture_manager_v1_interface kManagerImpl = {
    [](wl_client* client, wl_resource* m, uint32_t id, wl_resource* source, uint32_t options) {
        if (options & ~uint32_t(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS)) {
            wl_resource_post_error(m, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION, "invalid option");
            return;
        }
        create_session_resource(client, m, id, server_of(m), *source_of(source),
                                (options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) != 0, false);
    },
    [](wl_client* client, wl_resource* m, uint32_t id, wl_resource*, wl_resource*) {
        wl_resource* r = wl_resource_create(client, &ext_image_copy_capture_cursor_session_v1_interface,
                                            wl_resource_get_version(m), id);
        if (!r) {
            wl_client_post_no_memory(client);
            return;
        }
        auto* cs = new CursorSession{server_of(m), false};
        wl_resource_set_implementation(r, &kCursorImpl, cs, [](wl_resource* res) {
            delete static_cast<CursorSession*>(wl_resource_get_user_data(res));
        });
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

template <const wl_interface* Iface, auto Impl>
void bind_global(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, Iface, int(std::min<uint32_t>(version, 1)), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, Impl, data, nullptr);
}

}  // namespace

void Server::init_image_copy_capture() {
    image_copy_global =
        wl_global_create(display, &ext_image_copy_capture_manager_v1_interface, 1, this,
                         bind_global<&ext_image_copy_capture_manager_v1_interface, &kManagerImpl>);
    output_source_global =
        wl_global_create(display, &ext_output_image_capture_source_manager_v1_interface, 1, this,
                         bind_global<&ext_output_image_capture_source_manager_v1_interface, &kOutputSourceImpl>);
    toplevel_source_global = wl_global_create(
        display, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1, this,
        bind_global<&ext_foreign_toplevel_image_capture_source_manager_v1_interface, &kToplevelSourceImpl>);
}

}  // namespace brocompositor::wl
