// CPU access to a Linux SharedImage (ShmFd, or a LINEAR single-plane DmaBuf):
// for hosts without a GPU path (software rendering, uploads of shm client
// buffers) and for tests. DmaBuf mappings bracket the access with
// DMA_BUF_IOCTL_SYNC so caches are coherent with the GPU.
#pragma once

#include "brocompositor/surface.h"

#include <cstdint>
#include <memory>

namespace brocompositor::wl {

class CpuMapping {
public:
    // nullptr when the image cannot be mapped (tiled dmabuf, bad fd).
    static std::unique_ptr<CpuMapping> map(const SharedImage& image, bool write);
    ~CpuMapping();

    CpuMapping(const CpuMapping&) = delete;
    CpuMapping& operator=(const CpuMapping&) = delete;

    uint8_t* data() const { return data_; }  // first pixel of the image
    uint32_t stride() const { return stride_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // 0xAARRGGBB of a 32-bit pixel (formats with alpha ignored report 0xFF).
    uint32_t argb(uint32_t x, uint32_t y) const;
    void fill(uint32_t argb);
    void fill_rect(const Rect& r, uint32_t argb);

private:
    CpuMapping() = default;
    void* base_ = nullptr;
    size_t length_ = 0;
    uint8_t* data_ = nullptr;
    uint32_t stride_ = 0, width_ = 0, height_ = 0, fourcc_ = 0;
    int sync_fd_ = -1;  // dmabuf fd for DMA_BUF_IOCTL_SYNC, borrowed
    bool write_ = false;
};

}  // namespace brocompositor::wl
