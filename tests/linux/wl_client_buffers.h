// Buffers for the scripted test client: wl_shm (memfd) and linux-dmabuf
// (GBM on the compositor's main device, or udmabuf when GBM is unavailable),
// filled with a solid colour. Buffers are reused once the compositor
// releases them.
#pragma once

#include "linux-dmabuf-v1-client-protocol.h"

#include <wayland-client.h>

#include <cstdint>
#include <vector>

struct gbm_device;
struct gbm_bo;

namespace bctest {

struct DmabufFeedback {
    uint64_t main_device = 0;
    std::vector<std::pair<uint32_t, uint64_t>> table;  // format table
    std::vector<std::pair<uint32_t, uint64_t>> offered;  // tranche formats
    bool done = false;
    // Waits for the default feedback; false when none arrived.
    bool fetch(wl_display* display, zwp_linux_dmabuf_v1* dmabuf);
};

class BufferPool {
public:
    ~BufferPool() { clear(); }
    // Frees every buffer; call while the wl_display is still connected.
    void clear();
    wl_buffer* shm(wl_shm* shm, int w, int h, uint32_t argb);
    wl_buffer* dmabuf(zwp_linux_dmabuf_v1* dmabuf, const DmabufFeedback& fb, int w, int h, uint32_t argb);
    bool force_udmabuf = false;  // skip GBM: LINEAR system-memory dmabufs (CPU importers)
    uint64_t last_modifier = ~0ull;
    uint64_t last_modifier_reported = ~0ull;

    struct Buf {
        wl_buffer* buffer = nullptr;
        bool busy = false;
        bool is_dmabuf = false;
        int w = 0, h = 0;
        uint32_t color = 0;
        int fd = -1;          // memfd (shm) / dmabuf fd (linear)
        void* map = nullptr;  // shm mapping
        size_t size = 0;
        uint32_t stride = 0;
        gbm_bo* bo = nullptr;
    };

private:
    Buf* find_free(bool is_dmabuf, int w, int h);
    bool fill(Buf& b, uint32_t argb);  // false when the buffer cannot be CPU-mapped
    std::vector<Buf*> bufs_;
    gbm_device* gbm_ = nullptr;
    int gbm_fd_ = -1;
};

}  // namespace bctest
