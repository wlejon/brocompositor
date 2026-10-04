// Windows.Graphics.Capture of individual windows, delivered through the
// portable surface contract (brocompositor/surface.h).
//
// Each WindowCapture copies WGC frames into a small ring of D3D11 textures
// created with D3D11_RESOURCE_MISC_SHARED_NTHANDLE and exposes their NT
// handles (ImageHandleType::D3D11TextureNT). Copies are ordered by a shared
// ID3D11Fence (SyncHandleType::D3D11FenceNT): after copying frame N the
// capture signals the fence to Frame::wait_value, so a Vulkan host imports
// the fence once as a timeline semaphore and waits for that value before
// sampling. Leased images are never overwritten; when every free slot is
// taken the capture drops the frame (frames_dropped()).
//
// The D3D11 device must sit on the same adapter as the host's Vulkan device:
// pass the host's deviceLUID in CaptureDeviceConfig.
#pragma once

#include "brocompositor/surface.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace brocompositor::win {

struct CaptureDeviceConfig {
    std::optional<AdapterId> adapter;  // nullopt: the default hardware adapter
};

// One D3D11 device shared by any number of captures.
class CaptureDevice {
public:
    static std::shared_ptr<CaptureDevice> create(const CaptureDeviceConfig& config, std::string* error);
    ~CaptureDevice();

    AdapterId adapter() const;
    // ID3D11Device* (borrowed), for hosts that want to work in D3D11 directly.
    void* d3d11_device() const;

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    explicit CaptureDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct CaptureConfig {
    uint32_t ring_size = 3;      // >= 3: one being written, one latest, one leased
    bool capture_cursor = false;
    bool show_border = false;    // the yellow capture border (where the OS allows hiding it)
};

class WindowCapture final : public SurfaceSource {
public:
    // `window` is an HWND (WindowSnapshot::native).
    static std::unique_ptr<WindowCapture> start(std::shared_ptr<CaptureDevice> device, uint64_t window,
                                                const CaptureConfig& config, std::string* error);
    ~WindowCapture() override;

    std::optional<Frame> acquire() override;
    void release(const Frame& frame) override;
    std::vector<SharedImage> images() const override;
    std::optional<SharedImage> image(uint64_t id) const override;
    uint64_t images_generation() const override;
    SharedTimeline timeline() const override;
    bool closed() const override;

    // Called on a capture thread-pool thread after each new frame; keep it
    // cheap (post to the host's thread).
    void set_frame_callback(std::function<void()> callback);
    uint64_t frames_dropped() const;

    struct Impl;

private:
    explicit WindowCapture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Windows.Graphics.Capture is available on this system.
bool capture_supported();

}  // namespace brocompositor::win
