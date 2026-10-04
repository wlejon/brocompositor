// Optional Vulkan side of the surface contract: imports SharedImage /
// SharedTimeline handles into a host's VkDevice, zero-copy.
//
// The importer never links or loads a Vulkan loader; the host passes its own
// vkGetInstanceProcAddr (bro keeps owning its loader / volk). Requirements on
// the host's device:
//   * extensions from required_device_extensions()
//   * Vulkan 1.2 timelineSemaphore feature enabled
//   * the same adapter as the producer (compare adapter_of() with
//     SharedImage::adapter; on Windows pass it to CaptureDeviceConfig)
//
// Per frame the host waits on the imported timeline for Frame::wait_value
// (VkTimelineSemaphoreSubmitInfo, wait stage = where it first reads), wraps
// its first read of the image in cmd_acquire() (queue-family transfer from
// VK_QUEUE_FAMILY_EXTERNAL), and calls SurfaceSource::release() once that
// submission has completed.
#pragma once

#include "brocompositor/surface.h"

#include <vulkan/vulkan.h>

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
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
};

struct ImportedTimeline {
    uint64_t id = 0;  // SharedTimeline::id
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

// Device extensions needed for this platform's handle types.
std::vector<const char*> required_device_extensions();

// The physical device's identity in AdapterId terms (Windows: deviceLUID).
std::optional<AdapterId> adapter_of(VkPhysicalDevice physical_device, PFN_vkGetInstanceProcAddr gipa,
                                    VkInstance instance);

VkFormat to_vk_format(PixelFormat format);

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
