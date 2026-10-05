// Win32 half of the Vulkan importer: D3D11 NT textures and shared D3D11
// fences (timeline semaphores).
#include "vulkan/importer_impl.h"

#include <cstring>

namespace brocompositor::vk::platform {

std::vector<const char*> required_device_extensions() {
    return {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
}

std::optional<AdapterId> adapter_of(VkPhysicalDevice pd, PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
    auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (!props2) return std::nullopt;
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    props2(pd, &p);
    if (!id.deviceLUIDValid) return std::nullopt;
    AdapterId a;
    std::memcpy(a.luid.data(), id.deviceLUID, VK_LUID_SIZE);
    return a;
}

bool supports(const Importer::Impl& d, const SharedImage& image, VkImageUsageFlags usage) {
    if (image.type != ImageHandleType::D3D11TextureNT) return false;
    VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext};
    info.format = to_vk_format(image.format);
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ext_props};
    if (d.vkGetPhysicalDeviceImageFormatProperties2(d.ctx.physical_device, &info, &props) != VK_SUCCESS) return false;
    return (ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

bool supports(const Importer::Impl& d, const SharedTimeline& timeline) {
    if (timeline.type != SyncHandleType::D3D11FenceNT) return false;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    VkExternalSemaphoreProperties props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    d.vkGetPhysicalDeviceExternalSemaphoreProperties(d.ctx.physical_device, &info, &props);
    return (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
}

std::optional<ImportedImage> import_image(Importer& self, Importer::Impl& d, const SharedImage& image,
                                          VkImageUsageFlags usage, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ImportedImage> {
        if (error) *error = std::move(m);
        return std::nullopt;
    };
    if (image.type != ImageHandleType::D3D11TextureNT) return fail("unsupported image handle type");
    VkDevice dev = d.ctx.device;
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(image.handle.value));

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = to_vk_format(image.format);
    ici.extent = {image.width, image.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ImportedImage out;
    out.id = image.id;
    out.format = ici.format;
    out.extent = {image.width, image.height};
    out.alpha_ignored = image.format == PixelFormat::BGRX8Unorm || image.format == PixelFormat::RGBX8Unorm;
    VkResult r = d.vkCreateImage(dev, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return fail(vk_error("vkCreateImage", r));

    VkMemoryRequirements req{};
    d.vkGetImageMemoryRequirements(dev, out.image, &req);
    uint32_t bits = req.memoryTypeBits;
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (d.vkGetMemoryWin32HandlePropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, handle, &hp) ==
            VK_SUCCESS &&
        (bits & hp.memoryTypeBits))
        bits &= hp.memoryTypeBits;
    uint32_t type = d.pick_memory_type(bits);
    if (type == UINT32_MAX) {
        d.vkDestroyImage(dev, out.image, nullptr);
        return fail("no memory type accepts the shared texture");
    }

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = out.image;
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import.handle = handle;  // not consumed: the producer keeps owning the NT handle
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = d.vkAllocateMemory(dev, &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        d.vkDestroyImage(dev, out.image, nullptr);
        return fail(vk_error("vkAllocateMemory(import D3D11 texture)", r));
    }
    r = d.vkBindImageMemory(dev, out.image, out.memory, 0);
    if (r != VK_SUCCESS) {
        self.destroy(out);
        return fail(vk_error("vkBindImageMemory", r));
    }
    return out;
}

std::optional<ImportedTimeline> import_timeline(Importer::Impl& d, const SharedTimeline& timeline,
                                                std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ImportedTimeline> {
        if (error) *error = std::move(m);
        return std::nullopt;
    };
    if (timeline.type != SyncHandleType::D3D11FenceNT) return fail("unsupported sync handle type");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    ImportedTimeline out;
    out.id = timeline.id;
    VkResult r = d.vkCreateSemaphore(d.ctx.device, &sci, nullptr, &out.semaphore);
    if (r != VK_SUCCESS) return fail(vk_error("vkCreateSemaphore(timeline)", r));
    VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    import.semaphore = out.semaphore;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    import.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(timeline.handle.value));
    r = d.vkImportSemaphoreWin32HandleKHR(d.ctx.device, &import);
    if (r != VK_SUCCESS) {
        d.vkDestroySemaphore(d.ctx.device, out.semaphore, nullptr);
        return fail(vk_error("vkImportSemaphoreWin32HandleKHR(D3D11 fence)", r));
    }
    return out;
}

// The sync_file / dmabuf entry points are Linux-only.
std::vector<DrmFormat> dmabuf_formats(const Importer::Impl&, VkImageUsageFlags) { return {}; }

VkSemaphore import_sync_file(Importer::Impl&, NativeHandle, std::string* error) {
    if (error) *error = "sync_file import is Linux-only";
    return VK_NULL_HANDLE;
}

VkSemaphore create_exportable_semaphore(Importer::Impl&, std::string* error) {
    if (error) *error = "sync_file export is Linux-only";
    return VK_NULL_HANDLE;
}

NativeHandle export_sync_file(Importer::Impl&, VkSemaphore, std::string* error) {
    if (error) *error = "sync_file export is Linux-only";
    return NativeHandle{};
}

}  // namespace brocompositor::vk::platform
