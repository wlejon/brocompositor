// A minimal Vulkan host for the Linux import tests: dlopens the system
// loader, enumerates the devices that carry the importer's extensions
// (GPUs and lavapipe alike), and offers the two operations a compositor
// host needs: read an imported client image back after waiting its frame's
// sync_file, and render (clear) into an imported output image, returning a
// render-done sync_file for PresentRequest::render_done.
#pragma once

#include "brocompositor/vulkan/importer.h"

#include <optional>
#include <string>
#include <vector>

namespace bctest {

struct VkHostFns {
#define BC_VKH_FNS(X)                           \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkEnumerateDeviceExtensionProperties)     \
    X(vkCreateDevice)                           \
    X(vkGetDeviceProcAddr)                      \
    X(vkDestroyDevice)                          \
    X(vkGetDeviceQueue)                         \
    X(vkDeviceWaitIdle)                         \
    X(vkCreateCommandPool)                      \
    X(vkDestroyCommandPool)                     \
    X(vkAllocateCommandBuffers)                 \
    X(vkBeginCommandBuffer)                     \
    X(vkEndCommandBuffer)                       \
    X(vkResetCommandBuffer)                     \
    X(vkCmdCopyImageToBuffer)                   \
    X(vkCmdClearColorImage)                     \
    X(vkCmdPipelineBarrier)                     \
    X(vkQueueSubmit)                            \
    X(vkCreateFence)                            \
    X(vkDestroyFence)                           \
    X(vkWaitForFences)                          \
    X(vkResetFences)                            \
    X(vkCreateBuffer)                           \
    X(vkDestroyBuffer)                          \
    X(vkGetBufferMemoryRequirements)            \
    X(vkAllocateMemory)                         \
    X(vkFreeMemory)                             \
    X(vkBindBufferMemory)                       \
    X(vkMapMemory)                              \
    X(vkUnmapMemory)
#define BC_DECL(name) PFN_##name name = nullptr;
    BC_VKH_FNS(BC_DECL)
#undef BC_DECL
};

struct VkHostDevice {
    uint32_t index = 0;  // in vkEnumeratePhysicalDevices order
    std::string name;
    bool cpu = false;  // lavapipe / llvmpipe
    std::optional<brocompositor::AdapterId> adapter;
};

class VkHost {
public:
    VkHost() = default;
    VkHost(const VkHost&) = delete;
    VkHost& operator=(const VkHost&) = delete;
    ~VkHost();

    // Loads the loader and creates an instance; lists usable devices.
    bool load(std::string* why);
    std::vector<VkHostDevice> devices() const { return devices_; }
    // Creates a device on devices()[i] with the importer's extensions.
    bool open(size_t i, std::string* why);

    brocompositor::vk::DeviceContext device_context() const;
    const VkHostDevice& device() const { return devices_[open_]; }

    // Waits for `sync_fd` (may be none), acquires the image, copies it to a
    // buffer and returns tightly packed 4-byte texels in the image's VkFormat
    // order, or empty on failure.
    std::vector<uint8_t> read_back(brocompositor::vk::Importer& importer,
                                   const brocompositor::vk::ImportedImage& image, brocompositor::NativeHandle sync_fd,
                                   std::string* why);
    // Clears the whole image to `rgba` (0..1), releases it to the foreign
    // queue family, and returns the render-done sync_file (caller owns).
    std::optional<brocompositor::NativeHandle> clear(brocompositor::vk::Importer& importer,
                                                     const brocompositor::vk::ImportedImage& image,
                                                     const float rgba[4], std::string* why);

private:
    bool submit(VkSemaphore wait, VkSemaphore signal, std::string* why);
    void close_device();

    void* loader_ = nullptr;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    VkHostFns f_;
    VkInstance instance_ = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> physical_;
    std::vector<VkHostDevice> devices_;
    size_t open_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

}  // namespace bctest
