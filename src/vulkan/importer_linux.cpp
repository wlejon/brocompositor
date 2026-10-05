// Linux half of the Vulkan importer: dmabufs with explicit DRM format
// modifiers (VK_EXT_image_drm_format_modifier, disjoint planes when the
// planes are separate buffers), the importable format/modifier set, and
// sync_file semaphores (VK_KHR_external_semaphore_fd, SYNC_FD handles).
#include "vulkan/importer_impl.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <climits>

namespace brocompositor::vk::platform {

namespace {

constexpr uint64_t kModInvalid = 0x00ffffffffffffffull;  // DRM_FORMAT_MOD_INVALID
constexpr VkExternalMemoryHandleTypeFlagBits kDmaBuf = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// Formats offered to clients when the device can import them.
const uint32_t kCandidateFormats[] = {
    fourcc('A', 'R', '2', '4'), fourcc('X', 'R', '2', '4'), fourcc('A', 'B', '2', '4'), fourcc('X', 'B', '2', '4'),
    fourcc('A', 'R', '3', '0'), fourcc('X', 'R', '3', '0'), fourcc('A', 'B', '3', '0'), fourcc('X', 'B', '3', '0'),
    fourcc('R', 'G', '1', '6'), fourcc('A', 'B', '4', 'H'), fourcc('X', 'B', '4', 'H'),
};

bool fourcc_alpha_ignored(uint32_t f) {
    return f == fourcc('X', 'R', '2', '4') || f == fourcc('X', 'B', '2', '4') || f == fourcc('X', 'R', '3', '0') ||
           f == fourcc('X', 'B', '3', '0') || f == fourcc('X', 'B', '4', 'H') || f == fourcc('R', 'G', '1', '6');
}

int fd_of(NativeHandle h) { return h.value > uint64_t(INT_MAX) ? -1 : int(h.value); }

VkFormatFeatureFlags features_for(VkImageUsageFlags usage) {
    VkFormatFeatureFlags f = 0;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) f |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) f |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) f |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    return f;
}

std::vector<VkDrmFormatModifierPropertiesEXT> modifier_props(const Importer::Impl& d, VkFormat format) {
    VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
    d.vkGetPhysicalDeviceFormatProperties2(d.ctx.physical_device, format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> out(list.drmFormatModifierCount);
    if (out.empty()) return out;
    list.pDrmFormatModifierProperties = out.data();
    d.vkGetPhysicalDeviceFormatProperties2(d.ctx.physical_device, format, &props);
    out.resize(list.drmFormatModifierCount);
    return out;
}

// Whether `format` with `modifier` can be imported as a dmabuf with `usage`.
bool importable(const Importer::Impl& d, VkFormat format, uint64_t modifier, VkImageUsageFlags usage,
                VkImageCreateFlags flags) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    mod.drmFormatModifier = modifier;
    mod.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mod};
    ext.handleType = kDmaBuf;
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext};
    info.format = format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = usage;
    info.flags = flags;
    VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ext_props};
    if (d.vkGetPhysicalDeviceImageFormatProperties2(d.ctx.physical_device, &info, &props) != VK_SUCCESS) return false;
    return (ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

// Planes live in separate buffers when their fds name different files.
bool planes_disjoint(const SharedImage& image) {
    struct stat first {};
    if (fstat(fd_of(image.planes[0].handle), &first) != 0) return false;
    for (size_t i = 1; i < image.planes.size(); ++i) {
        struct stat st {};
        if (fstat(fd_of(image.planes[i].handle), &st) != 0) return true;
        if (st.st_ino != first.st_ino || st.st_dev != first.st_dev) return true;
    }
    return false;
}

VkFormat format_of(const SharedImage& image) {
    return image.drm_format ? vk_format_of_drm(image.drm_format) : to_vk_format(image.format);
}

}  // namespace

std::vector<const char*> required_device_extensions() {
    return {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
}

std::optional<AdapterId> adapter_of(VkPhysicalDevice pd, PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
    auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gipa(instance, "vkGetPhysicalDeviceProperties2"));
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    if (!props2 || !enum_ext) return std::nullopt;
    uint32_t n = 0;
    enum_ext(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    enum_ext(pd, nullptr, &n, exts.data());
    bool has_drm = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
        return std::string(e.extensionName) == VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
    });
    if (!has_drm) return std::nullopt;
    VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
    props2(pd, &p);
    if (!drm.hasRender) return std::nullopt;
    AdapterId a;
    a.drm_render_node = uint64_t(makedev(unsigned(drm.renderMajor), unsigned(drm.renderMinor)));
    return a;
}

bool supports(const Importer::Impl& d, const SharedImage& image, VkImageUsageFlags usage) {
    if (image.type != ImageHandleType::DmaBuf || image.planes.empty() || image.planes.size() > 4) return false;
    if (image.drm_modifier == kModInvalid) return false;
    VkFormat format = format_of(image);
    if (format == VK_FORMAT_UNDEFINED) return false;
    bool found = false;
    for (const auto& m : modifier_props(d, format)) {
        if (m.drmFormatModifier != image.drm_modifier) continue;
        if (m.drmFormatModifierPlaneCount != image.planes.size()) return false;
        if ((m.drmFormatModifierTilingFeatures & features_for(usage)) != features_for(usage)) return false;
        found = true;
    }
    if (!found) return false;
    VkImageCreateFlags flags = planes_disjoint(image) ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;
    return importable(d, format, image.drm_modifier, usage, flags);
}

bool supports(const Importer::Impl&, const SharedTimeline&) {
    // Linux surfaces carry per-frame sync files (import_sync_file), not a
    // shared timeline.
    return false;
}

std::optional<ImportedImage> import_image(Importer& self, Importer::Impl& d, const SharedImage& image,
                                          VkImageUsageFlags usage, std::string* error) {
    auto fail = [&](std::string m) -> std::optional<ImportedImage> {
        if (error) *error = std::move(m);
        return std::nullopt;
    };
    if (image.type != ImageHandleType::DmaBuf) return fail("unsupported image handle type (only dmabufs import)");
    const size_t n = image.planes.size();
    if (n == 0 || n > 4) return fail("a dmabuf needs 1-4 planes");
    if (image.drm_modifier == kModInvalid) return fail("dmabufs without an explicit modifier are not supported");
    VkFormat format = format_of(image);
    if (format == VK_FORMAT_UNDEFINED) return fail("no VkFormat for DRM format " + std::to_string(image.drm_format));
    for (const SharedPlane& p : image.planes)
        if (fd_of(p.handle) < 0) return fail("invalid plane fd");
    const bool disjoint = n > 1 && planes_disjoint(image);
    VkDevice dev = d.ctx.device;

    VkSubresourceLayout layouts[4]{};
    for (size_t i = 0; i < n; ++i) {
        layouts[i].offset = image.planes[i].offset;
        layouts[i].rowPitch = image.planes[i].stride;
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    mod.drmFormatModifier = image.drm_modifier;
    mod.drmFormatModifierPlaneCount = uint32_t(n);
    mod.pPlaneLayouts = layouts;
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod};
    ext.handleTypes = kDmaBuf;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ici.flags = disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {image.width, image.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    ImportedImage out;
    out.id = image.id;
    out.format = format;
    out.extent = {image.width, image.height};
    out.alpha_ignored = image.drm_format ? fourcc_alpha_ignored(image.drm_format)
                                         : (image.format == PixelFormat::BGRX8Unorm ||
                                            image.format == PixelFormat::RGBX8Unorm);
    out.external_queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT;
    VkResult r = d.vkCreateImage(dev, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return fail(vk_error("vkCreateImage(dmabuf)", r));

    const size_t mem_planes = disjoint ? n : 1;
    VkBindImageMemoryInfo binds[4]{};
    VkBindImagePlaneMemoryInfo plane_binds[4]{};
    static const VkImageAspectFlagBits kPlaneAspect[4] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    for (size_t i = 0; i < mem_planes; ++i) {
        // vkAllocateMemory takes ownership of the fd on success: import a dup.
        int fd = fcntl(fd_of(image.planes[i].handle), F_DUPFD_CLOEXEC, 0);
        if (fd < 0) {
            self.destroy(out);
            return fail("dup of a plane fd failed");
        }
        VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        r = d.vkGetMemoryFdPropertiesKHR(dev, kDmaBuf, fd, &fdp);
        if (r != VK_SUCCESS) {
            close(fd);
            self.destroy(out);
            return fail(vk_error("vkGetMemoryFdPropertiesKHR", r));
        }
        VkImagePlaneMemoryRequirementsInfo plane_req{VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO};
        plane_req.planeAspect = kPlaneAspect[i];
        VkImageMemoryRequirementsInfo2 req_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                                                disjoint ? &plane_req : nullptr};
        req_info.image = out.image;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        d.vkGetImageMemoryRequirements2(dev, &req_info, &req);
        uint32_t type = d.pick_memory_type(req.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits);
        if (type == UINT32_MAX) {
            close(fd);
            self.destroy(out);
            return fail("no memory type accepts the dmabuf");
        }
        off_t size = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);

        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = out.image;
        VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, disjoint ? nullptr : &dedicated};
        import.handleType = kDmaBuf;
        import.fd = fd;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
        mai.allocationSize = size > 0 ? VkDeviceSize(size) : req.memoryRequirements.size;
        mai.memoryTypeIndex = type;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        r = d.vkAllocateMemory(dev, &mai, nullptr, &mem);
        if (r != VK_SUCCESS) {
            close(fd);
            self.destroy(out);
            return fail(vk_error("vkAllocateMemory(import dmabuf)", r));
        }
        if (i == 0) out.memory = mem;
        else out.plane_memory[i - 1] = mem;

        binds[i] = VkBindImageMemoryInfo{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
        binds[i].image = out.image;
        binds[i].memory = mem;
        if (disjoint) {
            plane_binds[i] = VkBindImagePlaneMemoryInfo{VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO};
            plane_binds[i].planeAspect = kPlaneAspect[i];
            binds[i].pNext = &plane_binds[i];
        }
    }
    r = d.vkBindImageMemory2(dev, uint32_t(mem_planes), binds);
    if (r != VK_SUCCESS) {
        self.destroy(out);
        return fail(vk_error("vkBindImageMemory2(dmabuf)", r));
    }
    return out;
}

std::optional<ImportedTimeline> import_timeline(Importer::Impl&, const SharedTimeline&, std::string* error) {
    if (error) *error = "Linux surfaces have no shared timeline; use import_sync_file(frame.sync_fd)";
    return std::nullopt;
}

std::vector<DrmFormat> dmabuf_formats(const Importer::Impl& d, VkImageUsageFlags usage) {
    std::vector<DrmFormat> out;
    VkFormatFeatureFlags need = features_for(usage);
    for (uint32_t f : kCandidateFormats) {
        VkFormat format = vk_format_of_drm(f);
        DrmFormat df;
        df.fourcc = f;
        for (const auto& m : modifier_props(d, format)) {
            if ((m.drmFormatModifierTilingFeatures & need) != need) continue;
            VkImageCreateFlags flags = m.drmFormatModifierPlaneCount > 1 ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;
            // A multi-plane modifier may arrive with all planes in one buffer
            // (non-disjoint) or split; accept it when either imports.
            if (!importable(d, format, m.drmFormatModifier, usage, 0) &&
                !(flags && importable(d, format, m.drmFormatModifier, usage, flags)))
                continue;
            df.modifiers.push_back(m.drmFormatModifier);
        }
        if (!df.modifiers.empty()) out.push_back(std::move(df));
    }
    return out;
}

VkSemaphore import_sync_file(Importer::Impl& d, NativeHandle h, std::string* error) {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult r = d.vkCreateSemaphore(d.ctx.device, &sci, nullptr, &sem);
    if (r != VK_SUCCESS) {
        if (error) *error = vk_error("vkCreateSemaphore", r);
        return VK_NULL_HANDLE;
    }
    // -1 is a valid SYNC_FD payload meaning "already signalled".
    int fd = fd_of(h);
    int dup_fd = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (fd >= 0 && dup_fd < 0) {
        d.vkDestroySemaphore(d.ctx.device, sem, nullptr);
        if (error) *error = "dup of the sync_file failed";
        return VK_NULL_HANDLE;
    }
    VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    import.semaphore = sem;
    import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import.fd = dup_fd;
    r = d.vkImportSemaphoreFdKHR(d.ctx.device, &import);
    if (r != VK_SUCCESS) {
        if (dup_fd >= 0) close(dup_fd);
        d.vkDestroySemaphore(d.ctx.device, sem, nullptr);
        if (error) *error = vk_error("vkImportSemaphoreFdKHR(SYNC_FD)", r);
        return VK_NULL_HANDLE;
    }
    return sem;
}

VkSemaphore create_exportable_semaphore(Importer::Impl& d, std::string* error) {
    VkExportSemaphoreCreateInfo exp{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exp};
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult r = d.vkCreateSemaphore(d.ctx.device, &sci, nullptr, &sem);
    if (r != VK_SUCCESS) {
        if (error) *error = vk_error("vkCreateSemaphore(exportable)", r);
        return VK_NULL_HANDLE;
    }
    return sem;
}

NativeHandle export_sync_file(Importer::Impl& d, VkSemaphore semaphore, std::string* error) {
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    VkResult r = d.vkGetSemaphoreFdKHR(d.ctx.device, &info, &fd);
    if (r != VK_SUCCESS) {
        if (error) *error = vk_error("vkGetSemaphoreFdKHR(SYNC_FD)", r);
        return NativeHandle{~uint64_t(0)};
    }
    // fd -1: already signalled, nothing to wait for (same as "no fd").
    if (fd < 0) return NativeHandle{~uint64_t(0)};
    return NativeHandle{uint64_t(fd)};
}

}  // namespace brocompositor::vk::platform
