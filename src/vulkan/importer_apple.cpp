// Apple half of the Vulkan importer (MoltenVK, VK_EXT_metal_objects):
// IOSurface-backed images and MTLSharedEvent timelines.
//
// MoltenVK builds the VkImage's MTLTexture over the imported IOSurface
// itself; memory is still allocated and bound as for any image (dedicated).
// IOSurfaces have no queue-family ownership to transfer, so the image's
// external_queue_family is VK_QUEUE_FAMILY_IGNORED and cmd_acquire() /
// cmd_release() reduce to layout transitions.
#include "vulkan/importer_impl.h"

namespace brocompositor::vk::platform {

std::vector<const char*> required_device_extensions() { return {VK_EXT_METAL_OBJECTS_EXTENSION_NAME}; }

std::optional<AdapterId> adapter_of(VkPhysicalDevice pd, PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
    auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (!props2) return std::nullopt;
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    props2(pd, &p);
    if (!id.deviceLUIDValid) return std::nullopt;
    // MoltenVK stores MTLDevice.registryID big-endian in deviceLUID.
    AdapterId a;
    for (size_t i = 0; i < 8; ++i) {
        a.luid[i] = id.deviceLUID[i];
        a.metal_registry_id = a.metal_registry_id << 8 | id.deviceLUID[i];
    }
    return a;
}

bool supports(const Importer::Impl&, const SharedImage& image, VkImageUsageFlags) {
    return image.type == ImageHandleType::IOSurface && image.handle.value != 0 &&
           to_vk_format(image.format) != VK_FORMAT_UNDEFINED;
}

bool supports(const Importer::Impl&, const SharedTimeline& timeline) {
    return timeline.type == SyncHandleType::MetalSharedEvent && timeline.handle.value != 0;
}

std::optional<ImportedImage> import_image(Importer& self, Importer::Impl& d, const SharedImage& image,
                                          VkImageUsageFlags usage, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ImportedImage> {
        if (error) *error = std::move(m);
        return std::nullopt;
    };
    if (!supports(d, image, usage)) return fail("unsupported image handle type or format");
    VkDevice dev = d.ctx.device;
    VkImportMetalIOSurfaceInfoEXT ios{VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT};
    ios.ioSurface = reinterpret_cast<IOSurfaceRef>(static_cast<uintptr_t>(image.handle.value));
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ios};
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
    out.external_queue_family = VK_QUEUE_FAMILY_IGNORED;
    VkResult r = d.vkCreateImage(dev, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return fail(vk_error("vkCreateImage(IOSurface)", r));

    VkMemoryRequirements req{};
    d.vkGetImageMemoryRequirements(dev, out.image, &req);
    uint32_t type = d.pick_memory_type(req.memoryTypeBits);
    if (type == UINT32_MAX) {
        d.vkDestroyImage(dev, out.image, nullptr);
        return fail("no memory type for the IOSurface image");
    }
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = out.image;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = d.vkAllocateMemory(dev, &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        d.vkDestroyImage(dev, out.image, nullptr);
        return fail(vk_error("vkAllocateMemory(IOSurface image)", r));
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
    if (!supports(d, timeline)) return fail("unsupported sync handle type");
    VkImportMetalSharedEventInfoEXT ev{VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT};
    ev.mtlSharedEvent = reinterpret_cast<MTLSharedEvent_id>(static_cast<uintptr_t>(timeline.handle.value));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &ev};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    ImportedTimeline out;
    out.id = timeline.id;
    VkResult r = d.vkCreateSemaphore(d.ctx.device, &sci, nullptr, &out.semaphore);
    if (r != VK_SUCCESS) return fail(vk_error("vkCreateSemaphore(MTLSharedEvent timeline)", r));
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
