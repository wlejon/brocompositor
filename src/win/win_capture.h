#pragma once

#include "brocompositor/types.h"
#include <unordered_map>
#include <memory>
#include <mutex>

namespace brocompositor {

struct CaptureFrameInfo {
    WindowId window_id = InvalidWindowId;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgi_format = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
    uint64_t shared_handle = 0;
    uint64_t frame_index = 0;
};

class WinGraphicsCapture {
public:
    WinGraphicsCapture();
    ~WinGraphicsCapture();

    bool initialize();
    void shutdown();
    bool is_initialized() const { return initialized_; }

    bool start_capture(WindowId id);
    void stop_capture(WindowId id);
    bool is_capturing(WindowId id) const;

    uint64_t get_shared_texture_handle(WindowId id) const;
    CaptureFrameInfo get_frame_info(WindowId id) const;

private:
    struct Session {
        WindowId window_id = InvalidWindowId;
        uint64_t shared_handle = 0;
        void* d3d11_texture = nullptr;
        void* capture_session = nullptr;
        void* frame_pool = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t frame_index = 0;
        bool active = false;
    };

    mutable std::mutex mutex_;
    bool initialized_ = false;
    void* d3d11_device_ = nullptr;
    void* d3d11_context_ = nullptr;
    void* dxgi_device_ = nullptr;

    std::unordered_map<WindowId, Session> sessions_;
};

} // namespace brocompositor
