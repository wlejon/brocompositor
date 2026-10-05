// A minimal Vulkan host for the macOS tests: MoltenVK through the Vulkan
// loader (dlopen; nothing is linked), one queue, and a read-back of an
// imported image after waiting on an imported timeline.
#pragma once

#include "brocompositor/vulkan/importer.h"

#include <optional>
#include <string>
#include <vector>

namespace bctest {

class VkMac {
public:
    VkMac() = default;
    ~VkMac();
    // Picks the device matching `want` (any when null).
    bool init(const brocompositor::AdapterId* want, std::string* why);
    brocompositor::vk::DeviceContext context() const;
    const brocompositor::AdapterId& adapter() const { return adapter_; }

    // Waits for `timeline` >= `value` on the GPU, copies `image` into a host
    // buffer and returns its BGRA bytes (width * height * 4, tightly packed).
    std::optional<std::vector<uint8_t>> read(const brocompositor::vk::Importer& importer,
                                             const brocompositor::vk::ImportedImage& image, VkSemaphore timeline,
                                             uint64_t value, std::string* why);

private:
    void* loader_ = nullptr;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    brocompositor::AdapterId adapter_;
    PFN_vkGetDeviceProcAddr gdpa_ = nullptr;
    template <class T>
    T dev(const char* name) const {
        return reinterpret_cast<T>(gdpa_(device_, name));
    }
    template <class T>
    T inst(const char* name) const {
        return reinterpret_cast<T>(gipa_(instance_, name));
    }
};

}  // namespace bctest
