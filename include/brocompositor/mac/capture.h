// macOS window capture (ScreenCaptureKit), delivered through the portable
// surface contract (brocompositor/surface.h).
//
// Frames land in a SurfaceRing: a small ring of IOSurface-backed Metal
// textures owned by brocompositor (ImageHandleType::IOSurface). Each new
// frame is copied into a free slot by a Metal blit, which then signals a
// MTLSharedEvent to Frame::wait_value (SyncHandleType::MetalSharedEvent), so
// a Vulkan host (MoltenVK) imports the event once as a timeline semaphore
// and each slot once as an image, and waits for the value before sampling.
// Leased slots are never overwritten; when every free slot is taken the
// ring drops the frame (frames_dropped()).
//
// WindowCapture feeds a ring from a ScreenCaptureKit stream of one window
// and needs the Screen Recording permission (mac::query_permissions()). A
// SurfaceRing can also be fed directly with IOSurfaces (any producer, and
// the permission-free tests).
#pragma once

#include "brocompositor/surface.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace brocompositor::mac {

struct MetalDeviceConfig {
    // Pick the GPU with this MTLDevice.registryID (nullopt: the system
    // default). Compare with the host's vk::adapter_of().
    std::optional<AdapterId> adapter;
};

// One Metal device + command queue shared by any number of rings.
class MetalDevice {
public:
    static std::shared_ptr<MetalDevice> create(const MetalDeviceConfig& config, std::string* error);
    ~MetalDevice();

    AdapterId adapter() const;
    void* mtl_device() const;  // id<MTLDevice> (borrowed)

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    explicit MetalDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct RingConfig {
    uint32_t ring_size = 3;  // >= 3: one being written, one latest, one leased
};

class SurfaceRing final : public SurfaceSource {
public:
    static std::unique_ptr<SurfaceRing> create(std::shared_ptr<MetalDevice> device, const RingConfig& config,
                                               std::string* error);
    ~SurfaceRing() override;

    // Copies `content` pixels from the top-left of `iosurface` (an
    // IOSurfaceRef, BGRA 8-bit) into a free slot on the GPU and publishes it
    // as the newest frame once queued (the copy completes before the
    // timeline reaches the frame's wait_value). The slots follow the source
    // size: a different size makes a new image set. False when the frame was
    // dropped (every slot leased, unsupported pixel format, Metal failure).
    bool submit(void* iosurface, Size content, int64_t timestamp_ns);
    // No further frames (the producer is gone).
    void close();

    std::optional<Frame> acquire() override;
    void release(const Frame& frame) override;
    std::vector<SharedImage> images() const override;
    std::optional<SharedImage> image(uint64_t id) const override;
    uint64_t images_generation() const override;
    SharedTimeline timeline() const override;
    bool closed() const override;

    // Called on the submitting thread after each published frame.
    void set_frame_callback(std::function<void()> callback);
    uint64_t frames_dropped() const;

    struct Impl;

private:
    explicit SurfaceRing(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct CaptureConfig {
    uint32_t ring_size = 3;
    bool capture_cursor = false;
    uint32_t max_fps = 60;
};

// A ScreenCaptureKit stream of one window (CGWindowID, as in
// WindowSnapshot::native) into a SurfaceRing. The output follows the
// window's size in pixels (points x its display's scale) and is sRGB
// (BGRA8, sRGB-encoded) whatever the display's colour space.
//
// The whole window is captured wherever it is: a window parked at a display
// corner by the shell backend (a 1 pt sliver on screen) keeps delivering
// full frames with new content. A window that is not displayed at all
// (minimized, ordered out, closed, on another Space) delivers no frames;
// the capture stays open and resumes when the window is displayed again.
//
// closed() becomes true when the stream stops, when the window leaves the
// window server (its process exited), or, with the Accessibility
// permission, when its application no longer has the window (closed or
// ordered out: the same as the shell backend's WindowRemoved). macOS keeps
// a closed AppKit window in the window server, off screen, for as long as
// its process lives, and ScreenCaptureKit treats it exactly like an ordered
// out one, so without Accessibility closing a window is not detected.
class WindowCapture final : public SurfaceSource {
public:
    // Fails with a reason when Screen Recording is not granted or the window
    // is not shareable. Never prompts. The first frames can be from the
    // window's opening animation (a few percent smaller) when the window
    // was just created.
    static std::unique_ptr<WindowCapture> start(std::shared_ptr<MetalDevice> device, uint64_t window,
                                                const CaptureConfig& config, std::string* error);
    ~WindowCapture() override;

    std::optional<Frame> acquire() override;
    void release(const Frame& frame) override;
    std::vector<SharedImage> images() const override;
    std::optional<SharedImage> image(uint64_t id) const override;
    uint64_t images_generation() const override;
    SharedTimeline timeline() const override;
    bool closed() const override;

    // Called on a capture dispatch queue after each new frame; keep it cheap.
    void set_frame_callback(std::function<void()> callback);
    uint64_t frames_dropped() const;

    struct Impl;

private:
    explicit WindowCapture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// ScreenCaptureKit exists (macOS 12.3+) and Screen Recording is granted.
bool capture_supported();

}  // namespace brocompositor::mac
