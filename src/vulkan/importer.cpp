#if defined(_WIN32)
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif

#include "brocompositor/vulkan/importer.h"

#include <cstring>

namespace brocompositor::vk {

namespace {

#define BC_VK_INSTANCE_FNS(X)                       \
    X(vkGetDeviceProcAddr)                          \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceProperties2)               \
    X(vkGetPhysicalDeviceImageFormatProperties2)    \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)

#define BC_VK_DEVICE_FNS(X)              \
    X(vkCreateImage)                     \
    X(vkDestroyImage)                    \
    X(vkGetImageMemoryRequirements)      \
    X(vkAllocateMemory)                  \
    X(vkFreeMemory)                      \
    X(vkBindImageMemory)                 \
    X(vkCreateSemaphore)                 \
    X(vkDestroySemaphore)                \
    X(vkCmdPipelineBarrier)              \
    X(vkGetMemoryWin32HandlePropertiesKHR) \
    X(vkImportSemaphoreWin32HandleKHR)

std::string vk_error(const char* what, VkResult r) { return std::string(what) + " failed: VkResult " + std::to_string(int(r)); }

}  // namespace

struct Importer::Impl {
    DeviceContext ctx;
#define BC_DECL(name) PFN_##name name = nullptr;
    BC_VK_INSTANCE_FNS(BC_DECL)
    BC_VK_DEVICE_FNS(BC_DECL)
#undef BC_DECL
    VkPhysicalDeviceMemoryProperties memory{};
};

std::vector<const char*> required_device_extensions() {
#if defined(_WIN32)
    return {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
#else
    return {};
#endif
}

VkFormat to_vk_format(PixelFormat f) {
    switch (f) {
        case PixelFormat::BGRA8Unorm:
        case PixelFormat::BGRX8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::Unknown: break;
    }
    return VK_FORMAT_UNDEFINED;
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

Importer::Importer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Importer::~Importer() = default;

std::unique_ptr<Importer> Importer::create(const DeviceContext& ctx, std::string* error) {
    auto fail = [&](std::string m) -> std::unique_ptr<Importer> {
        if (error) *error = std::move(m);
        return nullptr;
    };
#if !defined(_WIN32)
    (void)ctx;
    return fail("the Vulkan importer implements the Win32 handle types only (Linux arrives with the server role)");
#else
    if (!ctx.get_instance_proc_addr || !ctx.instance || !ctx.physical_device || !ctx.device)
        return fail("incomplete DeviceContext");
    auto d = std::make_unique<Impl>();
    d->ctx = ctx;
#define BC_LOAD_I(name)                                                                              \
    d->name = reinterpret_cast<PFN_##name>(ctx.get_instance_proc_addr(ctx.instance, #name));        \
    if (!d->name) return fail("missing instance function " #name);
    BC_VK_INSTANCE_FNS(BC_LOAD_I)
#undef BC_LOAD_I
#define BC_LOAD_D(name)                                                                              \
    d->name = reinterpret_cast<PFN_##name>(d->vkGetDeviceProcAddr(ctx.device, #name));             \
    if (!d->name) return fail("missing device function " #name " (extension not enabled?)");
    BC_VK_DEVICE_FNS(BC_LOAD_D)
#undef BC_LOAD_D
    d->vkGetPhysicalDeviceMemoryProperties(ctx.physical_device, &d->memory);
    return std::unique_ptr<Importer>(new Importer(std::move(d)));
#endif
}

bool Importer::supports(const SharedImage& image, VkImageUsageFlags usage) const {
#if defined(_WIN32)
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
    if (impl_->vkGetPhysicalDeviceImageFormatProperties2(impl_->ctx.physical_device, &info, &props) != VK_SUCCESS)
        return false;
    return (ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
#else
    (void)image;
    (void)usage;
    return false;
#endif
}

bool Importer::supports(const SharedTimeline& timeline) const {
#if defined(_WIN32)
    if (timeline.type != SyncHandleType::D3D11FenceNT) return false;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    VkExternalSemaphoreProperties props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    impl_->vkGetPhysicalDeviceExternalSemaphoreProperties(impl_->ctx.physical_device, &info, &props);
    return (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
#else
    (void)timeline;
    return false;
#endif
}

std::optional<ImportedImage> Importer::import_image(const SharedImage& image, VkImageUsageFlags usage,
                                                    std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ImportedImage> {
        if (error) *error = std::move(m);
        return std::nullopt;
    };
#if defined(_WIN32)
    if (image.type != ImageHandleType::D3D11TextureNT) return fail("unsupported image handle type");
    auto& d = *impl_;
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
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < d.memory.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if (type == UINT32_MAX) type = i;
        if (d.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type = i;
            break;
        }
    }
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
        destroy(out);
        return fail(vk_error("vkBindImageMemory", r));
    }
    return out;
#else
    (void)image;
    (void)usage;
    return fail("unsupported platform");
#endif
}

std::optional<ImportedTimeline> Importer::import_timeline(const SharedTimeline& timeline, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ImportedTimeline> {
        if (error) *error = std::move(m);
        return std::nullopt;
    };
#if defined(_WIN32)
    if (timeline.type != SyncHandleType::D3D11FenceNT) return fail("unsupported sync handle type");
    auto& d = *impl_;
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
#else
    (void)timeline;
    return fail("unsupported platform");
#endif
}

void Importer::destroy(const ImportedImage& image) {
    if (image.image) impl_->vkDestroyImage(impl_->ctx.device, image.image, nullptr);
    if (image.memory) impl_->vkFreeMemory(impl_->ctx.device, image.memory, nullptr);
}

void Importer::destroy(const ImportedTimeline& timeline) {
    if (timeline.semaphore) impl_->vkDestroySemaphore(impl_->ctx.device, timeline.semaphore, nullptr);
}

void Importer::cmd_acquire(VkCommandBuffer cmd, const ImportedImage& image, uint32_t queue_family,
                           VkImageLayout new_layout, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) const {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = dst_access;
    // D3D11 has no layouts; the shared contents are defined in GENERAL.
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.dstQueueFamilyIndex = queue_family;
    b.image = image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl_->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void Importer::cmd_release(VkCommandBuffer cmd, const ImportedImage& image, uint32_t queue_family,
                           VkImageLayout current_layout, VkPipelineStageFlags src_stage, VkAccessFlags src_access) const {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = 0;
    b.oldLayout = current_layout;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = queue_family;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.image = image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl_->vkCmdPipelineBarrier(cmd, src_stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &b);
}

}  // namespace brocompositor::vk
