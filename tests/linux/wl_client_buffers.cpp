#include "linux/wl_client_buffers.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cstring>

namespace bctest {

namespace {

const zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
    [](void* data, zwp_linux_dmabuf_feedback_v1*) { static_cast<DmabufFeedback*>(data)->done = true; },
    [](void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t size) {
        auto* fb = static_cast<DmabufFeedback*>(data);
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            struct Entry {
                uint32_t format, pad;
                uint64_t modifier;
            };
            auto* e = static_cast<const Entry*>(p);
            for (uint32_t i = 0; i < size / sizeof(Entry); ++i) fb->table.emplace_back(e[i].format, e[i].modifier);
            munmap(p, size);
        }
        close(fd);
    },
    [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* dev) {
        auto* fb = static_cast<DmabufFeedback*>(data);
        if (dev->size >= sizeof(dev_t)) {
            dev_t d;
            std::memcpy(&d, dev->data, sizeof d);
            fb->main_device = uint64_t(d);
        }
    },
    [](void*, zwp_linux_dmabuf_feedback_v1*) {},
    [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {},
    [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
        auto* fb = static_cast<DmabufFeedback*>(data);
        const uint16_t* idx = static_cast<const uint16_t*>(indices->data);
        for (size_t i = 0; i < indices->size / sizeof(uint16_t); ++i)
            if (idx[i] < fb->table.size()) fb->offered.push_back(fb->table[idx[i]]);
    },
    [](void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {},
};

const wl_buffer_listener buffer_listener = {
    [](void* data, wl_buffer*) { static_cast<BufferPool::Buf*>(data)->busy = false; },
};

const zwp_linux_buffer_params_v1_listener params_listener = {
    [](void*, zwp_linux_buffer_params_v1*, wl_buffer*) {},
    [](void*, zwp_linux_buffer_params_v1*) {},
};

int open_render_node(uint64_t dev) {
    drmDevice* devices[16];
    int n = drmGetDevices2(0, devices, 16);
    int fd = -1;
    for (int i = 0; i < n && fd < 0; ++i) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) continue;
        struct stat st {};
        const char* path = devices[i]->nodes[DRM_NODE_RENDER];
        if (stat(path, &st) != 0) continue;
        bool match = uint64_t(st.st_rdev) == dev;
        if (!match && (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY))) {
            struct stat ps {};
            if (stat(devices[i]->nodes[DRM_NODE_PRIMARY], &ps) == 0) match = uint64_t(ps.st_rdev) == dev;
        }
        if (match) fd = open(path, O_RDWR | O_CLOEXEC);
    }
    drmFreeDevices(devices, n);
    return fd;
}

// A LINEAR dmabuf from system memory (no GPU needed).
int make_udmabuf(size_t size) {
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) return -1;
    int mem = memfd_create("bc-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mem < 0 || ftruncate(mem, off_t(size)) != 0) {
        if (mem >= 0) close(mem);
        close(dev);
        return -1;
    }
    fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK);
    struct udmabuf_create req {};
    req.memfd = uint32_t(mem);
    req.flags = UDMABUF_FLAGS_CLOEXEC;
    req.offset = 0;
    req.size = size;
    int fd = ioctl(dev, UDMABUF_CREATE, &req);
    close(mem);
    close(dev);
    return fd;
}

}  // namespace

bool DmabufFeedback::fetch(wl_display* display, zwp_linux_dmabuf_v1* dmabuf) {
    auto* f = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(f, &feedback_listener, this);
    for (int i = 0; i < 10 && !done; ++i) wl_display_roundtrip(display);
    zwp_linux_dmabuf_feedback_v1_destroy(f);
    return done && main_device != 0;
}

void BufferPool::clear() {
    for (Buf* b : bufs_) {
        if (b->buffer) wl_buffer_destroy(b->buffer);
        if (b->map) munmap(b->map, b->size);
        if (b->fd >= 0) close(b->fd);
        if (b->bo) gbm_bo_destroy(b->bo);
        delete b;
    }
    bufs_.clear();
    if (gbm_) gbm_device_destroy(gbm_);
    if (gbm_fd_ >= 0) close(gbm_fd_);
    gbm_ = nullptr;
    gbm_fd_ = -1;
}

BufferPool::Buf* BufferPool::find_free(bool is_dmabuf, int w, int h) {
    for (Buf* b : bufs_)
        if (!b->busy && b->is_dmabuf == is_dmabuf && b->w == w && b->h == h) return b;
    return nullptr;
}

bool BufferPool::fill(Buf& b, uint32_t argb) {
    b.color = argb;
    if (b.map) {
        for (int y = 0; y < b.h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(b.map) + size_t(y) * b.stride);
            std::fill(row, row + b.w, argb);
        }
        return true;
    }
    if (b.bo) {
        uint32_t stride = 0;
        void* data = nullptr;
        void* p = gbm_bo_map(b.bo, 0, 0, uint32_t(b.w), uint32_t(b.h), GBM_BO_TRANSFER_WRITE, &stride, &data);
        if (!p) return false;
        for (int y = 0; y < b.h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(p) + size_t(y) * stride);
            std::fill(row, row + b.w, argb);
        }
        gbm_bo_unmap(b.bo, data);
        return true;
    }
    if (b.fd >= 0) {  // linear udmabuf
        void* p = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
        if (p == MAP_FAILED) return false;
        dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
        ioctl(b.fd, DMA_BUF_IOCTL_SYNC, &s);
        for (int y = 0; y < b.h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(p) + size_t(y) * b.stride);
            std::fill(row, row + b.w, argb);
        }
        s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        ioctl(b.fd, DMA_BUF_IOCTL_SYNC, &s);
        munmap(p, b.size);
        return true;
    }
    return false;
}

wl_buffer* BufferPool::shm(wl_shm* shm, int w, int h, uint32_t argb) {
    Buf* b = find_free(false, w, h);
    if (!b) {
        b = new Buf();
        b->w = w;
        b->h = h;
        b->stride = uint32_t(w) * 4;
        b->size = size_t(b->stride) * uint32_t(h);
        b->fd = memfd_create("bc-shm", MFD_CLOEXEC);
        if (b->fd < 0 || ftruncate(b->fd, off_t(b->size)) != 0) {
            delete b;
            return nullptr;
        }
        b->map = mmap(nullptr, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
        wl_shm_pool* pool = wl_shm_create_pool(shm, b->fd, int32_t(b->size));
        b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, int32_t(b->stride), WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        wl_buffer_add_listener(b->buffer, &buffer_listener, b);
        bufs_.push_back(b);
    }
    fill(*b, argb);
    b->busy = true;
    return b->buffer;
}

wl_buffer* BufferPool::dmabuf(zwp_linux_dmabuf_v1* dmabuf, const DmabufFeedback& fb, int w, int h, uint32_t argb) {
    Buf* b = find_free(true, w, h);
    if (!b) {
        const uint32_t format = DRM_FORMAT_ARGB8888;
        std::vector<uint64_t> mods;
        for (auto& [f, m] : fb.offered)
            if (f == format) mods.push_back(m);
        if (mods.empty()) return nullptr;
        b = new Buf();
        b->w = w;
        b->h = h;
        b->is_dmabuf = true;
        std::vector<int> fds;
        std::vector<uint32_t> offsets, strides;
        uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
        if (!force_udmabuf && !gbm_ && gbm_fd_ < 0) {
            gbm_fd_ = open_render_node(fb.main_device);
            if (gbm_fd_ >= 0) gbm_ = gbm_create_device(gbm_fd_);
        }
        if (gbm_) {
            const bool linear_ok = std::count(mods.begin(), mods.end(), DRM_FORMAT_MOD_LINEAR) > 0;
            b->bo = gbm_bo_create_with_modifiers2(gbm_, uint32_t(w), uint32_t(h), format, mods.data(),
                                                  unsigned(mods.size()), GBM_BO_USE_RENDERING);
            // This client draws with the CPU: a tiled bo the driver cannot
            // map (NVIDIA block-linear) is replaced by a LINEAR one.
            if (b->bo && !fill(*b, argb)) {
                gbm_bo_destroy(b->bo);
                b->bo = nullptr;
            }
            if (!b->bo && linear_ok) {
                const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
                b->bo = gbm_bo_create_with_modifiers2(gbm_, uint32_t(w), uint32_t(h), format, &linear, 1,
                                                      GBM_BO_USE_RENDERING);
                if (!b->bo) b->bo = gbm_bo_create(gbm_, uint32_t(w), uint32_t(h), format, GBM_BO_USE_LINEAR);
            }
        }
        if (b->bo) {
            modifier = gbm_bo_get_modifier(b->bo);
            for (int i = 0; i < gbm_bo_get_plane_count(b->bo); ++i) {
                fds.push_back(gbm_bo_get_fd_for_plane(b->bo, i));
                offsets.push_back(gbm_bo_get_offset(b->bo, i));
                strides.push_back(gbm_bo_get_stride_for_plane(b->bo, i));
            }
        } else if (std::count(mods.begin(), mods.end(), DRM_FORMAT_MOD_LINEAR)) {
            b->stride = (uint32_t(w) * 4 + 63) & ~63u;
            b->size = (size_t(b->stride) * uint32_t(h) + 4095) & ~size_t(4095);
            b->fd = make_udmabuf(b->size);
            if (b->fd < 0) {
                delete b;
                return nullptr;
            }
            fds.push_back(b->fd);
            offsets.push_back(0);
            strides.push_back(b->stride);
        } else {
            delete b;
            return nullptr;
        }
        auto* params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        zwp_linux_buffer_params_v1_add_listener(params, &params_listener, nullptr);
        for (size_t i = 0; i < fds.size(); ++i)
            zwp_linux_buffer_params_v1_add(params, fds[i], uint32_t(i), offsets[i], strides[i], uint32_t(modifier >> 32),
                                           uint32_t(modifier & 0xffffffff));
        b->buffer = zwp_linux_buffer_params_v1_create_immed(params, w, h, format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (b->bo)
            for (int fd : fds) close(fd);
        wl_buffer_add_listener(b->buffer, &buffer_listener, b);
        bufs_.push_back(b);
        last_modifier = modifier;
    }
    if (!fill(*b, argb)) return nullptr;  // reported by the client as "error no-buffer"
    b->busy = true;
    return b->buffer;
}

}  // namespace bctest
