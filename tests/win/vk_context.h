// A minimal self-contained Vulkan host for the import test: loads the system
// loader dynamically, creates an instance/device with the importer's
// extensions and timeline semaphores, and offers just enough to copy an
// image into a host-visible buffer and read pixels back.
#pragma once

#include "brocompositor/vulkan/importer.h"

#include <string>

namespace bctest {

struct VkFns {
#define BC_VKT_FNS(X)                           \
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
    X(vkCreateCommandPool)                      \
    X(vkDestroyCommandPool)                     \
    X(vkAllocateCommandBuffers)                 \
    X(vkBeginCommandBuffer)                     \
    X(vkEndCommandBuffer)                       \
    X(vkResetCommandBuffer)                     \
    X(vkCmdCopyImageToBuffer)                   \
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
    BC_VKT_FNS(BC_DECL)
#undef BC_DECL
};

class VkContext {
public:
    ~VkContext();
    // Returns false with a reason when no suitable Vulkan device exists.
    bool init(std::string* why);

    brocompositor::vk::DeviceContext device_context() const;
    brocompositor::AdapterId adapter() const { return adapter_; }
    std::string device_name() const { return device_name_; }

    // Waits for `wait_value` on `timeline`, acquires `image`, copies it into
    // a buffer and returns BGRA pixels (width*height*4), or empty on failure.
    std::vector<uint8_t> read_back(brocompositor::vk::Importer& importer,
                                   const brocompositor::vk::ImportedImage& image,
                                   const brocompositor::vk::ImportedTimeline& timeline, uint64_t wait_value,
                                   std::string* why);

private:
    void* loader_ = nullptr;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    VkFns f_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    brocompositor::AdapterId adapter_;
    std::string device_name_;
};

}  // namespace bctest
