// The platform-neutral half of the Vulkan importer: function loading, format
// mapping, destruction, and the ownership-transfer barriers. Handle-type
// specifics live in importer_win32.cpp / importer_linux.cpp.
#include "vulkan/importer_impl.h"

namespace brocompositor::vk {

namespace {

// DRM fourcc codes (drm_fourcc.h, spelled out so Windows builds need no libdrm).
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

}  // namespace

uint32_t Importer::Impl::pick_memory_type(uint32_t bits) const {
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if (type == UINT32_MAX) type = i;
        if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return i;
    }
    return type;
}

std::vector<const char*> required_device_extensions() { return platform::required_device_extensions(); }

std::optional<AdapterId> adapter_of(VkPhysicalDevice pd, PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
    return platform::adapter_of(pd, gipa, instance);
}

VkFormat to_vk_format(PixelFormat f) {
    switch (f) {
        case PixelFormat::BGRA8Unorm:
        case PixelFormat::BGRX8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::RGBA8Unorm:
        case PixelFormat::RGBX8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::Other:
        case PixelFormat::Unknown: break;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat vk_format_of_drm(uint32_t f) {
    // DRM formats name components from the most significant bit of a
    // little-endian word, so ARGB8888 is bytes B,G,R,A = VK B8G8R8A8.
    if (f == fourcc('A', 'R', '2', '4') || f == fourcc('X', 'R', '2', '4')) return VK_FORMAT_B8G8R8A8_UNORM;
    if (f == fourcc('A', 'B', '2', '4') || f == fourcc('X', 'B', '2', '4')) return VK_FORMAT_R8G8B8A8_UNORM;
    if (f == fourcc('R', 'G', '1', '6')) return VK_FORMAT_R5G6B5_UNORM_PACK16;
    if (f == fourcc('A', 'R', '3', '0') || f == fourcc('X', 'R', '3', '0')) return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (f == fourcc('A', 'B', '3', '0') || f == fourcc('X', 'B', '3', '0')) return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    if (f == fourcc('A', 'B', '4', 'H') || f == fourcc('X', 'B', '4', 'H')) return VK_FORMAT_R16G16B16A16_SFLOAT;
    return VK_FORMAT_UNDEFINED;
}

Importer::Importer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Importer::~Importer() = default;

std::unique_ptr<Importer> Importer::create(const DeviceContext& ctx, std::string* error) {
    auto fail = [&](std::string m) -> std::unique_ptr<Importer> {
        if (error) *error = std::move(m);
        return nullptr;
    };
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
    BC_VK_PLATFORM_DEVICE_FNS(BC_LOAD_D)
#undef BC_LOAD_D
    d->vkGetPhysicalDeviceMemoryProperties(ctx.physical_device, &d->memory);
    return std::unique_ptr<Importer>(new Importer(std::move(d)));
}

bool Importer::supports(const SharedImage& image, VkImageUsageFlags usage) const {
    return platform::supports(*impl_, image, usage);
}

bool Importer::supports(const SharedTimeline& timeline) const { return platform::supports(*impl_, timeline); }

std::optional<ImportedImage> Importer::import_image(const SharedImage& image, VkImageUsageFlags usage,
                                                    std::string* error) {
    return platform::import_image(*this, *impl_, image, usage, error);
}

std::optional<ImportedTimeline> Importer::import_timeline(const SharedTimeline& timeline, std::string* error) {
    return platform::import_timeline(*impl_, timeline, error);
}

std::vector<DrmFormat> Importer::dmabuf_formats(VkImageUsageFlags usage) const {
    return platform::dmabuf_formats(*impl_, usage);
}

VkSemaphore Importer::import_sync_file(NativeHandle fd, std::string* error) {
    return platform::import_sync_file(*impl_, fd, error);
}

VkSemaphore Importer::create_exportable_semaphore(std::string* error) {
    return platform::create_exportable_semaphore(*impl_, error);
}

NativeHandle Importer::export_sync_file(VkSemaphore semaphore, std::string* error) {
    return platform::export_sync_file(*impl_, semaphore, error);
}

void Importer::destroy(VkSemaphore semaphore) {
    if (semaphore) impl_->vkDestroySemaphore(impl_->ctx.device, semaphore, nullptr);
}

void Importer::destroy(const ImportedImage& image) {
    if (image.image) impl_->vkDestroyImage(impl_->ctx.device, image.image, nullptr);
    if (image.memory) impl_->vkFreeMemory(impl_->ctx.device, image.memory, nullptr);
    for (VkDeviceMemory m : image.plane_memory)
        if (m) impl_->vkFreeMemory(impl_->ctx.device, m, nullptr);
}

void Importer::destroy(const ImportedTimeline& timeline) {
    if (timeline.semaphore) impl_->vkDestroySemaphore(impl_->ctx.device, timeline.semaphore, nullptr);
}

void Importer::cmd_acquire(VkCommandBuffer cmd, const ImportedImage& image, uint32_t queue_family,
                           VkImageLayout new_layout, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) const {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = dst_access;
    // External / foreign producers have no layouts: the shared contents are
    // defined in GENERAL.
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = image.external_queue_family;
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
    b.dstQueueFamilyIndex = image.external_queue_family;
    b.image = image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl_->vkCmdPipelineBarrier(cmd, src_stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &b);
}

}  // namespace brocompositor::vk
