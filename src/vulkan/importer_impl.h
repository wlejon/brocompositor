// Private: the importer's function table and the per-platform entry points
// (importer_win32.cpp / importer_linux.cpp) behind the common Importer.
#pragma once

// Every importer TU sees the same platform types (the Impl layout depends on them).
#if defined(_WIN32)
#include <windows.h>
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#endif

#include "brocompositor/vulkan/importer.h"

#include <string>

namespace brocompositor::vk {

#define BC_VK_INSTANCE_FNS(X)                       \
    X(vkGetDeviceProcAddr)                          \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceProperties2)               \
    X(vkGetPhysicalDeviceFormatProperties2)         \
    X(vkGetPhysicalDeviceImageFormatProperties2)    \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)

#define BC_VK_DEVICE_FNS(X)              \
    X(vkCreateImage)                     \
    X(vkDestroyImage)                    \
    X(vkGetImageMemoryRequirements)      \
    X(vkGetImageMemoryRequirements2)     \
    X(vkAllocateMemory)                  \
    X(vkFreeMemory)                      \
    X(vkBindImageMemory)                 \
    X(vkBindImageMemory2)                \
    X(vkCreateSemaphore)                 \
    X(vkDestroySemaphore)                \
    X(vkCmdPipelineBarrier)

#if defined(_WIN32)
#define BC_VK_PLATFORM_DEVICE_FNS(X)       \
    X(vkGetMemoryWin32HandlePropertiesKHR) \
    X(vkImportSemaphoreWin32HandleKHR)
#else
#define BC_VK_PLATFORM_DEVICE_FNS(X) \
    X(vkGetMemoryFdPropertiesKHR)    \
    X(vkImportSemaphoreFdKHR)        \
    X(vkGetSemaphoreFdKHR)
#endif

struct Importer::Impl {
    DeviceContext ctx;
#define BC_DECL(name) PFN_##name name = nullptr;
    BC_VK_INSTANCE_FNS(BC_DECL)
    BC_VK_DEVICE_FNS(BC_DECL)
    BC_VK_PLATFORM_DEVICE_FNS(BC_DECL)
#undef BC_DECL
    VkPhysicalDeviceMemoryProperties memory{};

    // Index of a memory type allowed by `bits`, preferring device-local;
    // UINT32_MAX when none.
    uint32_t pick_memory_type(uint32_t bits) const;
};

inline std::string vk_error(const char* what, VkResult r) {
    return std::string(what) + " failed: VkResult " + std::to_string(int(r));
}

// Per-platform halves.
namespace platform {
std::vector<const char*> required_device_extensions();
std::optional<AdapterId> adapter_of(VkPhysicalDevice pd, PFN_vkGetInstanceProcAddr gipa, VkInstance instance);
bool supports(const Importer::Impl& d, const SharedImage& image, VkImageUsageFlags usage);
bool supports(const Importer::Impl& d, const SharedTimeline& timeline);
std::optional<ImportedImage> import_image(Importer& self, Importer::Impl& d, const SharedImage& image,
                                          VkImageUsageFlags usage, std::string* error);
std::optional<ImportedTimeline> import_timeline(Importer::Impl& d, const SharedTimeline& timeline,
                                                std::string* error);
std::vector<DrmFormat> dmabuf_formats(const Importer::Impl& d, VkImageUsageFlags usage);
VkSemaphore import_sync_file(Importer::Impl& d, NativeHandle fd, std::string* error);
VkSemaphore create_exportable_semaphore(Importer::Impl& d, std::string* error);
NativeHandle export_sync_file(Importer::Impl& d, VkSemaphore semaphore, std::string* error);
}  // namespace platform

}  // namespace brocompositor::vk
