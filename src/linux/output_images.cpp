// Output images: what the host renders into. Each is a wlr_buffer of our own
// (no wlr_allocator, no wlr_renderer) backed by
//   * GBM buffer objects on the backend's DRM device (scanout-capable on KMS),
//   * DRM dumb buffers when GBM cannot allocate (e.g. vkms), or
//   * memfd shm, for CPU hosts and nested parents without dmabuf.
// The host sees them as SharedImages (DmaBuf planes / ShmFd).
#include "linux/drm_util.h"
#include "linux/server_impl.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cstring>

namespace brocompositor::wl {

struct OutputImageSlot {
    struct Buf {  // standard layout: base first
        wlr_buffer base;
        OutputImageSlot* slot;
    } buf{};
    Server* srv = nullptr;
    MonitorId output = kNoMonitor;
    SharedImage desc;
    std::vector<int> fds;
    gbm_bo* bo = nullptr;
    int dumb_fd = -1;         // device the dumb buffer lives on (borrowed)
    uint32_t dumb_handle = 0;
    void* map = nullptr;      // shm mapping (data_ptr access)
    size_t map_size = 0;
    Listener release;

    ~OutputImageSlot() {
        if (map) munmap(map, map_size);
        for (int fd : fds)
            if (fd >= 0) ::close(fd);
        if (bo) gbm_bo_destroy(bo);
        if (dumb_handle) {
            drm_mode_destroy_dumb d{};
            d.handle = dumb_handle;
            drmIoctl(dumb_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
    }
};

namespace {

OutputImageSlot* slot_of(wlr_buffer* b) { return reinterpret_cast<OutputImageSlot::Buf*>(b)->slot; }

void slot_destroy(wlr_buffer* b) {
    OutputImageSlot* s = slot_of(b);
    s->release.disconnect();
    delete s;
}

bool slot_get_dmabuf(wlr_buffer* b, wlr_dmabuf_attributes* a) {
    OutputImageSlot* s = slot_of(b);
    if (s->desc.type != ImageHandleType::DmaBuf) return false;
    std::memset(a, 0, sizeof *a);
    a->width = int32_t(s->desc.width);
    a->height = int32_t(s->desc.height);
    a->format = s->desc.drm_format;
    a->modifier = s->desc.drm_modifier;
    a->n_planes = int(s->desc.planes.size());
    for (size_t i = 0; i < s->desc.planes.size() && i < WLR_DMABUF_MAX_PLANES; ++i) {
        a->fd[i] = fd_of(s->desc.planes[i].handle);
        a->offset[i] = s->desc.planes[i].offset;
        a->stride[i] = s->desc.planes[i].stride;
    }
    return true;
}

bool slot_get_shm(wlr_buffer* b, wlr_shm_attributes* a) {
    OutputImageSlot* s = slot_of(b);
    if (s->desc.type != ImageHandleType::ShmFd) return false;
    a->fd = fd_of(s->desc.planes[0].handle);
    a->format = s->desc.drm_format;
    a->width = int(s->desc.width);
    a->height = int(s->desc.height);
    a->stride = int(s->desc.planes[0].stride);
    a->offset = s->desc.planes[0].offset;
    return true;
}

bool slot_begin_ptr(wlr_buffer* b, uint32_t, void** data, uint32_t* format, size_t* stride) {
    OutputImageSlot* s = slot_of(b);
    if (!s->map) return false;
    *data = s->map;
    *format = s->desc.drm_format;
    *stride = s->desc.planes[0].stride;
    return true;
}

void slot_end_ptr(wlr_buffer*) {}

const wlr_buffer_impl kSlotImpl = {slot_destroy, slot_get_dmabuf, slot_get_shm, slot_begin_ptr, slot_end_ptr};

std::vector<uint64_t> modifiers_for(const ServerConfig& cfg, uint32_t format, const wlr_drm_format_set* scanout) {
    std::vector<uint64_t> host;
    for (const auto& f : cfg.dmabuf_formats)
        if (f.fourcc == format) host = f.modifiers;
    std::vector<uint64_t> out;
    if (scanout) {
        const wlr_drm_format* sf = wlr_drm_format_set_get(scanout, format);
        if (!sf) return {};
        for (size_t i = 0; i < sf->len; ++i) {
            uint64_t m = sf->modifiers[i];
            if (m == DRM_FORMAT_MOD_INVALID) continue;
            if (host.empty() || std::find(host.begin(), host.end(), m) != host.end()) out.push_back(m);
        }
        return out;
    }
    if (!host.empty()) return host;
    return {DRM_FORMAT_MOD_LINEAR};
}

}  // namespace

static OutputImageSlot* new_slot(Server* srv, OutputRec& out) {
    auto* s = new OutputImageSlot();
    s->srv = srv;
    s->output = out.id;
    s->buf.slot = s;
    s->desc.id = next_image_id();
    s->desc.adapter = srv->adapter;
    return s;
}

static OutputImageSlot* alloc_shm(Server* srv, OutputRec& out, int w, int h, uint32_t format) {
    uint32_t stride = (uint32_t(w) * 4 + 63) & ~63u;
    size_t size = size_t(stride) * uint32_t(h);
    int fd = create_memfd("brocompositor-output", size);
    if (fd < 0) return nullptr;
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return nullptr;
    }
    OutputImageSlot* s = new_slot(srv, out);
    s->fds.push_back(fd);
    s->map = map;
    s->map_size = size;
    s->desc.type = ImageHandleType::ShmFd;
    s->desc.width = uint32_t(w);
    s->desc.height = uint32_t(h);
    s->desc.drm_format = format;
    s->desc.format = pixel_format_of(format);
    s->desc.planes.push_back(SharedPlane{from_fd(fd), 0, stride});
    return s;
}

static OutputImageSlot* alloc_gbm(Server* srv, OutputRec& out, gbm_device* gbm, int w, int h, uint32_t format,
                                  const std::vector<uint64_t>& mods, bool scanout) {
    uint32_t flags = GBM_BO_USE_RENDERING | (scanout ? GBM_BO_USE_SCANOUT : 0);
    gbm_bo* bo = nullptr;
    bool linear_only = mods.size() == 1 && mods[0] == DRM_FORMAT_MOD_LINEAR;
    if (!mods.empty() && !linear_only)
        bo = gbm_bo_create_with_modifiers2(gbm, uint32_t(w), uint32_t(h), format, mods.data(), unsigned(mods.size()),
                                           flags);
    bool forced_linear = false;
    if (!bo && (mods.empty() || linear_only || std::count(mods.begin(), mods.end(), DRM_FORMAT_MOD_LINEAR))) {
        bo = gbm_bo_create(gbm, uint32_t(w), uint32_t(h), format, flags | GBM_BO_USE_LINEAR);
        forced_linear = bo != nullptr;
    }
    if (!bo) return nullptr;
    OutputImageSlot* s = new_slot(srv, out);
    s->bo = bo;
    s->desc.type = ImageHandleType::DmaBuf;
    s->desc.width = uint32_t(w);
    s->desc.height = uint32_t(h);
    s->desc.drm_format = format;
    s->desc.format = pixel_format_of(format);
    s->desc.drm_modifier = gbm_bo_get_modifier(bo);
    // GBM_BO_USE_LINEAR guarantees the layout even when the driver reports
    // the implicit modifier (kms_swrast on vkms does): name it, so hosts can
    // map and import the image.
    if (forced_linear && s->desc.drm_modifier == DRM_FORMAT_MOD_INVALID) s->desc.drm_modifier = DRM_FORMAT_MOD_LINEAR;
    int planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < planes; ++i) {
        // Export read-write (gbm_bo_get_fd_for_plane is read-only, which
        // refuses a host's writable CPU mapping of a LINEAR image).
        int fd = -1;
        gbm_bo_handle h = gbm_bo_get_handle_for_plane(bo, i);
        if (drmPrimeHandleToFD(gbm_device_get_fd(gbm), h.u32, DRM_CLOEXEC | DRM_RDWR, &fd) != 0) fd = -1;
        if (fd < 0) fd = gbm_bo_get_fd_for_plane(bo, i);
        if (fd < 0) {
            delete s;
            return nullptr;
        }
        s->fds.push_back(fd);
        s->desc.planes.push_back(SharedPlane{from_fd(fd), gbm_bo_get_offset(bo, i), gbm_bo_get_stride_for_plane(bo, i)});
    }
    return s;
}

static OutputImageSlot* alloc_dumb(Server* srv, OutputRec& out, int drm_fd, int w, int h, uint32_t format) {
    drm_mode_create_dumb c{};
    c.width = uint32_t(w);
    c.height = uint32_t(h);
    c.bpp = 32;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0) return nullptr;
    int fd = -1;
    if (drmPrimeHandleToFD(drm_fd, c.handle, DRM_CLOEXEC | DRM_RDWR, &fd) != 0) {
        drm_mode_destroy_dumb d{};
        d.handle = c.handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return nullptr;
    }
    OutputImageSlot* s = new_slot(srv, out);
    s->dumb_fd = drm_fd;
    s->dumb_handle = c.handle;
    s->fds.push_back(fd);
    s->desc.type = ImageHandleType::DmaBuf;
    s->desc.width = uint32_t(w);
    s->desc.height = uint32_t(h);
    s->desc.drm_format = format;
    s->desc.format = pixel_format_of(format);
    s->desc.drm_modifier = DRM_FORMAT_MOD_LINEAR;
    s->desc.planes.push_back(SharedPlane{from_fd(fd), 0, c.pitch});
    return s;
}

bool Server::ensure_output_images(OutputRec& out, int w, int h) {
    if (!out.slots.empty() && int(out.slots[0]->desc.width) == w && int(out.slots[0]->desc.height) == h)
        return true;
    free_output_images(out);
    uint32_t format = config.output_format;
    bool is_drm = wlr_output_is_drm(out.output);
    int backend_fd = wlr_backend_get_drm_fd(backend);

    bool want_dmabuf = false;
    switch (config.output_buffers) {
        case OutputBufferKind::Shm: want_dmabuf = false; break;
        case OutputBufferKind::DmaBuf: want_dmabuf = true; break;
        case OutputBufferKind::Auto: want_dmabuf = is_drm || backend_gbm || gbm; break;
    }
    if (is_drm) want_dmabuf = true;  // KMS scans out dmabufs only
    if (out.use_shm && !is_drm) want_dmabuf = false;

    gbm_device* dev = backend_gbm ? backend_gbm : gbm;
    const wlr_drm_format_set* scan = is_drm ? wlr_output_get_primary_formats(out.output, WLR_BUFFER_CAP_DMABUF) : nullptr;
    std::vector<uint64_t> mods = modifiers_for(config, format, scan);

    uint32_t n = std::max<uint32_t>(2, config.output_image_count);
    for (uint32_t i = 0; i < n; ++i) {
        OutputImageSlot* s = nullptr;
        if (want_dmabuf && dev) s = alloc_gbm(this, out, dev, w, h, format, mods, is_drm);
        if (!s && want_dmabuf && is_drm && backend_fd >= 0) s = alloc_dumb(this, out, backend_fd, w, h, format);
        if (!s && want_dmabuf && !is_drm && render_fd >= 0 && config.output_buffers == OutputBufferKind::DmaBuf)
            s = alloc_dumb(this, out, render_fd, w, h, format);
        if (!s && !is_drm && config.output_buffers != OutputBufferKind::DmaBuf) s = alloc_shm(this, out, w, h, format);
        if (!s) {
            wlr_log(WLR_ERROR, "brocompositor: cannot allocate %dx%d output images", w, h);
            free_output_images(out);
            return false;
        }
        wlr_buffer_init(&s->buf.base, &kSlotImpl, w, h);
        uint64_t image_id = s->desc.id;
        MonitorId oid = out.id;
        s->release.connect(&s->buf.base.events.release,
                           [this, oid, image_id](void*) { set_image_state(oid, image_id, SlotState::Free); });
        out.slots.push_back(s);
    }
    ++out.images_generation;
    publish_output_images(out);
    return true;
}

void Server::free_output_images(OutputRec& out) {
    set_front_image(out, 0);
    for (OutputImageSlot* s : out.slots) wlr_buffer_drop(&s->buf.base);
    out.slots.clear();
    out.pending_image = 0;
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.outputs.find(out.id);
        if (it != mirror.outputs.end()) it->second.images.clear();
    }
}

void Server::publish_output_images(OutputRec& out) {
    std::lock_guard<std::mutex> lock(mirror.m);
    OutputMirror& m = mirror.outputs[out.id];
    m.images.clear();
    for (OutputImageSlot* s : out.slots) m.images.push_back(OutputMirror::Image{s->desc, SlotState::Free});
    m.info.images_generation = out.images_generation;
}

OutputImageSlot* Server::slot(OutputRec& out, uint64_t image_id) {
    for (OutputImageSlot* s : out.slots)
        if (s->desc.id == image_id) return s;
    return nullptr;
}

void Server::set_front_image(OutputRec& out, uint64_t image_id) {
    // A backend may release a committed buffer at once (headless does): the
    // slot would go back to the host, which repaints it from the background
    // up while a capture still copies it as the screen. Lock the new front
    // before unlocking the old, so presenting the same image twice is safe.
    OutputImageSlot* next = image_id ? slot(out, image_id) : nullptr;
    if (next) wlr_buffer_lock(&next->buf.base);
    if (OutputImageSlot* prev = out.front_image ? slot(out, out.front_image) : nullptr)
        wlr_buffer_unlock(&prev->buf.base);
    out.front_image = next ? image_id : 0;
}

void Server::set_image_state(MonitorId output, uint64_t image_id, SlotState state) {
    std::lock_guard<std::mutex> lock(mirror.m);
    auto it = mirror.outputs.find(output);
    if (it == mirror.outputs.end()) return;
    for (auto& img : it->second.images)
        if (img.desc.id == image_id) img.state = state;
}

// wlr_buffer of a slot, for the present path in outputs.cpp.
wlr_buffer* slot_buffer(OutputImageSlot* s) { return &s->buf.base; }
const SharedImage& slot_desc(OutputImageSlot* s) { return s->desc; }

}  // namespace brocompositor::wl
