#include "brocompositor/linux/cpu_mapping.h"

#include "linux/drm_util.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace brocompositor::wl {

std::unique_ptr<CpuMapping> CpuMapping::map(const SharedImage& image, bool write) {
    if (image.planes.size() != 1) return nullptr;
    if (image.type == ImageHandleType::DmaBuf && image.drm_modifier != DRM_FORMAT_MOD_LINEAR) return nullptr;
    if (image.type != ImageHandleType::DmaBuf && image.type != ImageHandleType::ShmFd) return nullptr;
    uint32_t fourcc = image.drm_format;
    if (bytes_per_pixel(fourcc) != 4) return nullptr;
    int fd = int(image.planes[0].handle.value);
    if (fd < 0) return nullptr;
    const SharedPlane& p = image.planes[0];
    size_t need = size_t(p.offset) + size_t(p.stride) * image.height;
    size_t length = need;
    if (image.type == ImageHandleType::ShmFd) {
        struct stat st {};
        if (fstat(fd, &st) == 0 && size_t(st.st_size) >= need) length = size_t(st.st_size);
    } else {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end > 0 && size_t(end) >= need) length = size_t(end);
    }
    int prot = PROT_READ | (write ? PROT_WRITE : 0);
    void* base = mmap(nullptr, length, prot, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) return nullptr;
    auto m = std::unique_ptr<CpuMapping>(new CpuMapping());
    m->base_ = base;
    m->length_ = length;
    m->data_ = static_cast<uint8_t*>(base) + p.offset;
    m->stride_ = p.stride;
    m->width_ = image.width;
    m->height_ = image.height;
    m->fourcc_ = fourcc;
    m->write_ = write;
    if (image.type == ImageHandleType::DmaBuf) {
        m->sync_fd_ = fd;
        dma_buf_sync s{};
        s.flags = DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ);
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
    }
    return m;
}

CpuMapping::~CpuMapping() {
    if (sync_fd_ >= 0) {
        dma_buf_sync s{};
        s.flags = DMA_BUF_SYNC_END | (write_ ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ);
        ioctl(sync_fd_, DMA_BUF_IOCTL_SYNC, &s);
    }
    if (base_) munmap(base_, length_);
}

uint32_t CpuMapping::argb(uint32_t x, uint32_t y) const {
    if (x >= width_ || y >= height_) return 0;
    uint32_t v;
    std::memcpy(&v, data_ + size_t(y) * stride_ + size_t(x) * 4, 4);
    switch (fourcc_) {
        case DRM_FORMAT_ARGB8888: return v;
        case DRM_FORMAT_XRGB8888: return v | 0xFF000000u;
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888: {
            uint32_t r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
            uint32_t a = fourcc_ == DRM_FORMAT_XBGR8888 ? 0xFF : v >> 24;
            return a << 24 | r << 16 | g << 8 | b;
        }
        default: return v;
    }
}

void CpuMapping::fill_rect(const Rect& r, uint32_t argb) {
    uint32_t v = argb;
    if (fourcc_ == DRM_FORMAT_ABGR8888 || fourcc_ == DRM_FORMAT_XBGR8888) {
        uint32_t a = argb >> 24, rr = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
        v = a << 24 | b << 16 | g << 8 | rr;
    }
    int32_t x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int32_t x1 = std::min<int32_t>(int32_t(width_), r.right()), y1 = std::min<int32_t>(int32_t(height_), r.bottom());
    for (int32_t y = y0; y < y1; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(data_ + size_t(y) * stride_);
        std::fill(row + x0, row + std::max(x0, x1), v);
    }
}

void CpuMapping::fill(uint32_t argb) { fill_rect(Rect{0, 0, int32_t(width_), int32_t(height_)}, argb); }

}  // namespace brocompositor::wl
