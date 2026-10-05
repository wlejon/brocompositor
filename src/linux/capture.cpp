// Screen capture core, shared by wlr-screencopy and ext-image-copy-capture.
// The source of an output capture is the output image the host presented
// (the server has no renderer of its own). A copy is made on the server
// thread when both sides are CPU-mappable (shm output images, LINEAR dmabufs,
// shm client buffers); anything else - tiled images, dmabuf client buffers,
// window sources, or ServerConfig::host_capture_copies - becomes a
// CaptureRequest that the host answers with capture_done().
#include "brocompositor/linux/cpu_mapping.h"
#include "linux/capture_util.h"
#include "linux/drm_util.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace brocompositor::wl {

wlr_buffer* slot_buffer(OutputImageSlot* s);
const SharedImage& slot_desc(OutputImageSlot* s);

namespace {

// Byte order of a 32-bit format: is it xRGB-like (B at byte 0) or xBGR-like?
// -1 for formats the CPU path does not convert.
int rgb_order(uint32_t fourcc) {
    switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888: return 0;
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888: return 1;
        default: return -1;
    }
}

uint32_t swap_rb(uint32_t p) { return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); }

// Copies `region` of `src` to the top-left of the client buffer. False when
// either side is not CPU accessible or the formats are not convertible.
bool cpu_copy(const SharedImage& src, const Rect& region, wlr_buffer* dst) {
    int so = rgb_order(src.drm_format);
    if (so < 0) return false;
    auto map = CpuMapping::map(src, false);
    if (!map) return false;
    void* data = nullptr;
    uint32_t dfmt = 0;
    size_t dstride = 0;
    if (!wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &dfmt, &dstride)) return false;
    int dord = rgb_order(dfmt);
    if (dord < 0) {
        wlr_buffer_end_data_ptr_access(dst);
        return false;
    }
    bool swap = dord != so;
    bool opaque = !fourcc_has_alpha(src.drm_format) && fourcc_has_alpha(dfmt);
    int x0 = std::max(0, region.x), y0 = std::max(0, region.y);
    int x1 = std::min<int>(int(map->width()), region.x + region.width);
    int y1 = std::min<int>(int(map->height()), region.y + region.height);
    // Where the clipped region lands in the target.
    int dx = x0 - region.x, dy = y0 - region.y;
    int w = std::min(x1 - x0, dst->width - dx), h = std::min(y1 - y0, dst->height - dy);
    for (int y = 0; y < h; ++y) {
        const auto* s = reinterpret_cast<const uint32_t*>(map->data() + size_t(y0 + y) * map->stride()) + x0;
        auto* d = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data) + size_t(dy + y) * dstride) + dx;
        if (!swap && !opaque) {
            std::memcpy(d, s, size_t(std::max(0, w)) * 4);
            continue;
        }
        for (int x = 0; x < w; ++x) {
            uint32_t p = s[x];
            if (swap) p = swap_rb(p);
            if (opaque) p |= 0xFF000000u;
            d[x] = p;
        }
    }
    wlr_buffer_end_data_ptr_access(dst);
    return true;
}

// The client buffer as a SharedImage for the host (fds dup()ed into `fds`).
bool describe_target(wlr_buffer* b, SharedImage* out, std::vector<int>* fds) {
    wlr_dmabuf_attributes dma{};
    if (wlr_buffer_get_dmabuf(b, &dma)) {
        out->type = ImageHandleType::DmaBuf;
        out->width = uint32_t(dma.width);
        out->height = uint32_t(dma.height);
        out->drm_format = dma.format;
        out->format = pixel_format_of(dma.format);
        out->drm_modifier = dma.modifier;
        for (int i = 0; i < dma.n_planes; ++i) {
            int fd = fcntl(dma.fd[i], F_DUPFD_CLOEXEC, 3);
            fds->push_back(fd);
            out->planes.push_back(SharedPlane{from_fd(fd), dma.offset[i], dma.stride[i]});
        }
        out->id = next_image_id();
        return true;
    }
    wlr_shm_attributes shm{};
    if (wlr_buffer_get_shm(b, &shm)) {
        out->type = ImageHandleType::ShmFd;
        out->width = uint32_t(shm.width);
        out->height = uint32_t(shm.height);
        out->drm_format = shm.format;
        out->format = pixel_format_of(shm.format);
        int fd = fcntl(shm.fd, F_DUPFD_CLOEXEC, 3);
        fds->push_back(fd);
        out->planes.push_back(SharedPlane{from_fd(fd), uint32_t(shm.offset), uint32_t(shm.stride)});
        out->id = next_image_id();
        return true;
    }
    return false;
}

void release_job(CaptureJob& job) {
    for (int fd : job.fds)
        if (fd >= 0) ::close(fd);
    job.fds.clear();
    job.buffer_destroy.disconnect();
    if (job.buffer) wlr_buffer_unlock(job.buffer);
    job.buffer = nullptr;
}

void finish_job(std::unique_ptr<CaptureJob> job, bool ok) {
    if (job->finish) job->finish(*job, ok);
    release_job(*job);
}

}  // namespace

// ---------------------------------------------------------------- capture_util.h

OutputCaptureInfo output_capture_info(OutputRec& out) {
    OutputCaptureInfo i;
    if (!out.slots.empty()) {
        const SharedImage& d = slot_desc(out.slots.front());
        i.drm_format = d.drm_format;
        i.width = int(d.width);
        i.height = int(d.height);
    } else {
        i.drm_format = DRM_FORMAT_XRGB8888;
        i.width = out.output->width;
        i.height = out.output->height;
    }
    return i;
}

OutputImageSlot* front_slot(Server& s, OutputRec& out) {
    return out.front_image ? s.slot(out, out.front_image) : nullptr;
}

uint32_t drm_to_shm(uint32_t f) {
    if (f == DRM_FORMAT_ARGB8888) return WL_SHM_FORMAT_ARGB8888;
    if (f == DRM_FORMAT_XRGB8888) return WL_SHM_FORMAT_XRGB8888;
    return f;
}

bool capture_buffer_ok(wlr_buffer* b, uint32_t drm_format, int w, int h) {
    if (!b || b->width != w || b->height != h) return false;
    wlr_shm_attributes shm{};
    if (wlr_buffer_get_shm(b, &shm)) return shm.format == drm_format;
    wlr_dmabuf_attributes dma{};
    if (wlr_buffer_get_dmabuf(b, &dma)) return dma.format == drm_format;
    return false;
}

void DamageAccum::add(const std::vector<Rect>& damage) {
    if (full || damage_is_none(damage)) return;
    if (damage.empty() || rects.size() + damage.size() > 32) {
        full = true;
        rects.clear();
        return;
    }
    for (const Rect& r : damage)
        if (!r.empty()) rects.push_back(r);
}

std::vector<Rect> DamageAccum::take(int w, int h) {
    std::vector<Rect> out = full ? std::vector<Rect>{Rect{0, 0, w, h}} : std::move(rects);
    full = false;
    rects.clear();
    return out;
}

void track_output_damage(OutputRec& out, std::weak_ptr<DamageAccum> acc) {
    out.present_waiters.push_back([acc](OutputImageSlot* image, const std::vector<Rect>& damage, int64_t) {
        auto a = acc.lock();
        if (!a || !image) return true;
        a->add(damage);
        return false;
    });
}

int64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// ---------------------------------------------------------------- jobs

void Server::run_output_capture(std::unique_ptr<CaptureJob> job, OutputImageSlot* source) {
    if (!source || !job->buffer) return finish_job(std::move(job), false);
    const SharedImage& src = slot_desc(source);
    if (!config.host_capture_copies && cpu_copy(src, job->region, job->buffer))
        return finish_job(std::move(job), true);
    if (!describe_target(job->buffer, &job->target, &job->fds)) return finish_job(std::move(job), false);
    CaptureRequest req;
    req.id = job->id = next_capture++;
    req.output = job->output;
    req.source_image = src.id;
    req.region = job->region;
    req.with_cursor = job->cursor;
    req.target = job->target;
    captures[req.id] = std::move(job);
    server_events.push(req);
}

void Server::run_window_capture(std::unique_ptr<CaptureJob> job) {
    if (!job->buffer || !window_ref(job->window) || locked()) return finish_job(std::move(job), false);
    if (!describe_target(job->buffer, &job->target, &job->fds)) return finish_job(std::move(job), false);
    CaptureRequest req;
    req.id = job->id = next_capture++;
    req.window = job->window;
    req.scale = job->scale;
    req.with_cursor = job->cursor;
    req.target = job->target;
    captures[req.id] = std::move(job);
    server_events.push(req);
}

void Server::capture_done(uint64_t id, bool ok, int render_done_fd) {
    if (render_done_fd >= 0) {
        // The client reads its buffer as soon as it hears `ready`.
        wait_sync_file(render_done_fd, 1000);
        ::close(render_done_fd);
    }
    auto it = captures.find(id);
    if (it == captures.end()) return;
    std::unique_ptr<CaptureJob> job = std::move(it->second);
    captures.erase(it);
    // A window captured while the session got locked is not handed out.
    if (job->window != kNoWindow && locked()) ok = false;
    finish_job(std::move(job), ok);
}

void Server::notify_presented(OutputRec& out, OutputImageSlot* image, const std::vector<Rect>& damage,
                              int64_t when_ns) {
    if (out.present_waiters.empty()) return;
    std::vector<PresentWaiter> waiters;
    waiters.swap(out.present_waiters);
    std::vector<PresentWaiter> keep;
    for (PresentWaiter& w : waiters)
        if (!w(image, damage, when_ns)) keep.push_back(std::move(w));
    // Waiters added while calling the others are already in present_waiters.
    for (PresentWaiter& w : keep) out.present_waiters.push_back(std::move(w));
}

void Server::notify_window_commit(WindowId window) {
    if (window_commit_waiters.empty()) return;
    std::vector<std::pair<WindowId, std::function<bool()>>> waiters;
    waiters.swap(window_commit_waiters);
    for (auto& w : waiters)
        if (w.first != window || !w.second()) window_commit_waiters.push_back(std::move(w));
}

void Server::fail_output_waiters(OutputRec& out) {
    std::vector<PresentWaiter> waiters;
    waiters.swap(out.present_waiters);
    for (PresentWaiter& w : waiters) w(nullptr, {}, 0);
}

void Server::shutdown_captures() {
    for (auto& [id, out] : outputs) fail_output_waiters(*out);
    auto jobs = std::move(captures);
    captures.clear();
    for (auto& [id, job] : jobs) finish_job(std::move(job), false);
    window_commit_waiters.clear();
}

}  // namespace brocompositor::wl
