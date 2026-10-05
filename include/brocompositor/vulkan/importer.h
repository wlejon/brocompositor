// Optional Vulkan side of the surface contract: imports SharedImage /
// SharedTimeline handles into a host's VkDevice, zero-copy.
//
// The importer never links or loads a Vulkan loader; the host passes its own
// vkGetInstanceProcAddr (bro keeps owning its loader / volk). Requirements on
// the host's device:
//   * extensions from required_device_extensions()
//   * Vulkan 1.2 (timelineSemaphore enabled on Windows)
//   * the same adapter as the producer (compare adapter_of() with
//     SharedImage::adapter; on Windows pass it to CaptureDeviceConfig)
//
// Windows (D3D11 capture): per frame the host waits on the imported timeline
// for Frame::wait_value (VkTimelineSemaphoreSubmitInfo, wait stage = where it
// first reads), wraps its first read in cmd_acquire() (queue-family transfer
// from VK_QUEUE_FAMILY_EXTERNAL), and calls SurfaceSource::release() once
// that submission has completed.
//
// Linux (Wayland server role): images are dmabufs with an explicit DRM format
// modifier (imported with VK_EXT_image_drm_format_modifier, disjoint when the
// planes live in different buffers). Per frame the host waits on
// import_sync_file(frame.sync_fd) in its submit, wraps the first read in
// cmd_acquire() (transfer from VK_QUEUE_FAMILY_FOREIGN_EXT), and releases the
// lease after the submission completes. To render into a server output image
// it imports it the same way, signals a semaphore from
// create_exportable_semaphore() in its submit, and passes export_sync_file()
// of it as PresentRequest::render_done. Advertise dmabuf_formats() to the
// server (ServerConfig::dmabuf_formats) so clients only allocate what the
// host can import. wl_shm images (ImageHandleType::ShmFd) are not imported:
// upload them from a CPU mapping.
//
// macOS (MoltenVK; ScreenCaptureKit capture / mac::SurfaceRing): images are
// IOSurfaces (VK_EXT_metal_objects; the host also enables
// VK_KHR_portability_subset as MoltenVK requires), the timeline a
// MTLSharedEvent imported as a timeline semaphore. Per frame the host waits
// for Frame::wait_value as on Windows; cmd_acquire() is a plain layout
// transition (external_queue_family is VK_QUEUE_FAMILY_IGNORED).
#pragma once

#include "brocompositor/surface.h"

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::vk {

struct DeviceContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
};

struct ImportedImage {
    uint64_t id = 0;  // SharedImage::id
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;  // the only memory, or memory plane 0 when disjoint
    std::array<VkDeviceMemory, 3> plane_memory{};  // disjoint dmabuf: memory planes 1..3
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    // The format's alpha channel is undefined (XRGB8888, BGRX...): sample
    // with an A = ONE swizzle.
    bool alpha_ignored = false;
    // Queue family the producer owns the image in (EXTERNAL on Windows,
    // FOREIGN_EXT for dmabufs, IGNORED for IOSurfaces: no transfer);
    // cmd_acquire / cmd_release transfer from / to it.
    uint32_t external_queue_family = VK_QUEUE_FAMILY_EXTERNAL;
};

struct ImportedTimeline {
    uint64_t id = 0;  // SharedTimeline::id
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

// A DRM fourcc and the modifiers the device can import it with.
struct DrmFormat {
    uint32_t fourcc = 0;
    std::vector<uint64_t> modifiers;
};

// Device extensions needed for this platform's handle types.
std::vector<const char*> required_device_extensions();

// The physical device's identity in AdapterId terms (Windows: deviceLUID;
// Linux: the render node from VK_EXT_physical_device_drm, nullopt when the
// driver lacks it, e.g. lavapipe; macOS: the MTLDevice registryID MoltenVK
// encodes in deviceLUID).
std::optional<AdapterId> adapter_of(VkPhysicalDevice physical_device, PFN_vkGetInstanceProcAddr gipa,
                                    VkInstance instance);

VkFormat to_vk_format(PixelFormat format);
// DRM fourcc -> VkFormat (byte-order aware); UNDEFINED when unmapped.
VkFormat vk_format_of_drm(uint32_t fourcc);

class Importer {
public:
    static std::unique_ptr<Importer> create(const DeviceContext& context, std::string* error);
    ~Importer();

    // Whether the driver can import this image / timeline with the given usage.
    bool supports(const SharedImage& image, VkImageUsageFlags usage) const;
    bool supports(const SharedTimeline& timeline) const;

    std::optional<ImportedImage> import_image(const SharedImage& image, VkImageUsageFlags usage,
                                              std::string* error);
    std::optional<ImportedTimeline> import_timeline(const SharedTimeline& timeline, std::string* error);
    void destroy(const ImportedImage& image);
    void destroy(const ImportedTimeline& timeline);

    // Linux: formats and modifiers importable as dmabufs with `usage`.
    std::vector<DrmFormat> dmabuf_formats(VkImageUsageFlags usage) const;
    // Linux: a binary semaphore holding a sync_file's fence (temporary
    // import of a dup; the caller keeps its fd). Wait on it in one submit,
    // then destroy it. An invalid fd yields an already-signalled semaphore.
    VkSemaphore import_sync_file(NativeHandle fd, std::string* error);
    // Linux: a binary semaphore whose signal can be exported as a sync_file.
    VkSemaphore create_exportable_semaphore(std::string* error);
    // Linux: the sync_file of the pending signal of `semaphore` (submit the
    // signalling batch first). The caller owns the returned fd; ~0 (no fd)
    // when the signal already completed or on error (`error` set).
    NativeHandle export_sync_file(VkSemaphore semaphore, std::string* error);
    void destroy(VkSemaphore semaphore);

    // Ownership transfer external -> queue_family, layout -> new_layout.
    void cmd_acquire(VkCommandBuffer cmd, const ImportedImage& image, uint32_t queue_family,
                     VkImageLayout new_layout, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) const;
    // Hands the image back to the external producer.
    void cmd_release(VkCommandBuffer cmd, const ImportedImage& image, uint32_t queue_family,
                     VkImageLayout current_layout, VkPipelineStageFlags src_stage,
                     VkAccessFlags src_access) const;

    struct Impl;

private:
    explicit Importer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace brocompositor::vk
