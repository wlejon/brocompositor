// The surface contract: how window contents reach the host's GPU.
//
// A SurfaceSource (a captured foreign window on Windows; a client surface of
// the Wayland server on Linux) exposes a small set of OS-native, shareable GPU
// images and a synchronization primitive. The host imports each image once
// (keyed by SharedImage::id) into its own graphics API and then, per frame,
// leases the newest one:
//
//   auto frame = source.acquire();            // leases frame.image_id
//   if (frame) {
//       VkImage img = cache.get_or_import(source.image(frame->image_id));
//       // GPU-wait sync before reading (Windows: timeline value on the
//       // source's shared fence; Linux: the per-frame sync fd)
//       ... sample img ...
//       // once the host's GPU work that reads img has completed:
//       source.release(*frame);
//   }
//
// Handle ownership: every NativeHandle in this header is owned by the source.
// Windows NT handles stay valid until the image is retired (see
// images_generation()) and all its leases are released; importing a Win32
// handle into Vulkan does not transfer ownership, and the imported memory
// remains valid after the source closes its handle. POSIX fds (Linux) stay
// valid while the image is listed or leased; the host dup()s them before an
// import that takes ownership (the Vulkan importer does).
//
// An image is never written while leased, so a host that holds a lease can
// sample it for as long as it wants; the source drops frames rather than
// overwrite a leased image.
#pragma once

#include "brocompositor/geometry.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace brocompositor {

enum class PixelFormat : uint32_t {
    Unknown = 0,
    BGRA8Unorm = 1,  // DXGI_FORMAT_B8G8R8A8_UNORM / VK_FORMAT_B8G8R8A8_UNORM / DRM ARGB8888
    RGBA8Unorm = 2,
    BGRX8Unorm = 3,  // alpha ignored (DRM XRGB8888)
    RGBX8Unorm = 4,  // alpha ignored (DRM XBGR8888)
    Other = 5,       // see SharedImage::drm_format
};

enum class ImageHandleType : uint32_t {
    None = 0,
    D3D11TextureNT = 1,  // NT handle from IDXGIResource1::CreateSharedHandle
                         //  -> VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
    DmaBuf = 2,          // Linux: per-plane fds + DRM format modifier
    ShmFd = 3,           // Linux: wl_shm pool fd + offset/stride (CPU upload path)
};

enum class SyncHandleType : uint32_t {
    None = 0,
    D3D11FenceNT = 1,  // shared ID3D11Fence; a timeline the host waits on with
                       // Frame::wait_value (VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT
                       // imported as a VK_SEMAPHORE_TYPE_TIMELINE semaphore)
    SyncFileFd = 2,    // Linux: a per-frame sync_file in Frame::sync_fd
};

// Identifies the GPU an image lives on. The host must import on the same
// adapter: compare with VkPhysicalDeviceIDProperties::deviceLUID (Windows) or
// the DRM render node (Linux).
struct AdapterId {
    std::array<uint8_t, 8> luid{};  // Windows LUID, little-endian {LowPart, HighPart}
    uint64_t drm_render_node = 0;   // Linux: st_rdev of the render node
    bool operator==(const AdapterId&) const = default;
};

struct NativeHandle {
    uint64_t value = 0;  // HANDLE on Windows, fd on POSIX (0 / -1 meaning none per platform)
    bool operator==(const NativeHandle&) const = default;
};

struct SharedPlane {
    NativeHandle handle;
    uint32_t offset = 0;
    uint32_t stride = 0;
};

struct SharedImage {
    uint64_t id = 0;  // unique for the process lifetime; import cache key
    ImageHandleType type = ImageHandleType::None;
    NativeHandle handle;            // single-handle types (D3D11TextureNT)
    std::vector<SharedPlane> planes;  // DmaBuf / ShmFd
    uint64_t drm_modifier = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::Unknown;
    uint32_t drm_format = 0;  // Linux: DRM fourcc (authoritative there; `format` is its coarse mapping)
    AdapterId adapter;
};

struct SharedTimeline {
    SyncHandleType type = SyncHandleType::None;
    NativeHandle handle;
    uint64_t id = 0;  // import cache key
};

struct Frame {
    uint64_t sequence = 0;  // increases with every new frame the source produced
    uint64_t image_id = 0;
    Size content;           // valid region from (0,0); may be smaller than the image
    std::vector<Rect> damage;  // in image pixels; empty means "everything"
    uint64_t wait_value = 0;   // D3D11FenceNT: GPU-wait for timeline >= wait_value
    NativeHandle sync_fd;      // SyncFileFd: per-frame fence
    int64_t timestamp_ns = 0;  // presentation/arrival time, source clock
};

class SurfaceSource {
public:
    virtual ~SurfaceSource() = default;

    // Leases the newest frame (which may be the one already returned last
    // time; compare Frame::sequence). nullopt before the first frame.
    virtual std::optional<Frame> acquire() = 0;
    // Ends a lease. Call only after the host's GPU work reading the image has
    // completed.
    virtual void release(const Frame& frame) = 0;

    // Current image set. When it changes (resize), images_generation() bumps;
    // the host drops imports whose id is no longer listed once it holds no
    // lease on them.
    virtual std::vector<SharedImage> images() const = 0;
    virtual std::optional<SharedImage> image(uint64_t id) const = 0;
    virtual uint64_t images_generation() const = 0;
    virtual SharedTimeline timeline() const = 0;

    // The host presented content from this source (drives Wayland frame
    // callbacks on Linux; a no-op for Windows capture).
    virtual void presented(int64_t /*timestamp_ns*/) {}

    // The underlying window/surface is gone; no further frames will arrive.
    virtual bool closed() const = 0;
};

}  // namespace brocompositor
