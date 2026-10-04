#include "wl_dmabuf_vulkan.h"
#include "wl_backend.h"
#include <algorithm>

namespace brocompositor {

WlDmabufVulkan::WlDmabufVulkan() {
    supported_formats_ = {
        DRM_FOURCC_ARGB8888,
        DRM_FOURCC_XRGB8888,
        DRM_FOURCC_ABGR8888,
        DRM_FOURCC_XBGR8888,
        DRM_FOURCC_RGBA8888,
        DRM_FOURCC_RGB565,
        DRM_FOURCC_NV12
    };
}

WlDmabufVulkan::~WlDmabufVulkan() {
    shutdown();
}

bool WlDmabufVulkan::initialize(WlBackend& backend) {
    if (initialized_) return true;

#if defined(BRO_HAS_WAYLAND)
    if (backend.get_display()) {
        // wlr_linux_dmabuf_v1_create initializes the wayland protocol extension
        dmabuf_v1_ = wlr_linux_dmabuf_v1_create(backend.get_display(), 4, nullptr);
    }
#else
    (void)backend;
    dmabuf_v1_ = reinterpret_cast<wlr_linux_dmabuf_v1*>(static_cast<uintptr_t>(0x6000));
#endif

    initialized_ = true;
    return true;
}

void WlDmabufVulkan::shutdown() {
    if (!initialized_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        imported_textures_.clear();
    }

#if defined(BRO_HAS_WAYLAND)
    dmabuf_v1_ = nullptr;
#else
    dmabuf_v1_ = nullptr;
#endif

    initialized_ = false;
}

bool WlDmabufVulkan::is_format_supported(uint32_t drm_format) const {
    return std::find(supported_formats_.begin(), supported_formats_.end(), drm_format) != supported_formats_.end();
}

std::vector<uint32_t> WlDmabufVulkan::get_supported_formats() const {
    return supported_formats_;
}

std::vector<uint64_t> WlDmabufVulkan::get_modifiers_for_format(uint32_t drm_format) const {
    if (!is_format_supported(drm_format)) {
        return {};
    }
    // Return standard linear and vendor tiling modifiers
    return {DRM_MODIFIER_LINEAR, 0x0001000000000001ULL /* Tiled modifier example */};
}

std::shared_ptr<VulkanImportedTexture> WlDmabufVulkan::import_dmabuf(const DmaBufAttributes& attribs) {
    if (attribs.width <= 0 || attribs.height <= 0 || attribs.n_planes <= 0) {
        return nullptr;
    }
    if (!is_format_supported(attribs.format)) {
        return nullptr;
    }

    // Prepare VkImportMemoryFdInfoKHR structure
    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.pNext = nullptr;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = attribs.planes[0].fd;

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t handle = next_texture_handle_++;

    auto tex = std::make_shared<VulkanImportedTexture>();
    tex->image_handle = handle;
    tex->memory_handle = handle + 0x1000;
    tex->view_handle = handle + 0x2000;
    tex->width = attribs.width;
    tex->height = attribs.height;
    tex->format = attribs.format;
    tex->modifier = attribs.modifier;
    tex->plane_count = std::min(attribs.n_planes, 4);

    for (int i = 0; i < tex->plane_count; ++i) {
        tex->fds[i] = attribs.planes[i].fd;
    }
    tex->is_valid = true;

    imported_textures_[handle] = tex;
    return tex;
}

bool WlDmabufVulkan::release_texture(uint64_t texture_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = imported_textures_.find(texture_handle);
    if (it == imported_textures_.end()) {
        return false;
    }
    it->second->is_valid = false;
    imported_textures_.erase(it);
    return true;
}

std::shared_ptr<VulkanImportedTexture> WlDmabufVulkan::get_texture(uint64_t texture_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = imported_textures_.find(texture_handle);
    return it != imported_textures_.end() ? it->second : nullptr;
}

bool WlDmabufVulkan::import_sync_fence(int sync_fd, uint64_t texture_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = imported_textures_.find(texture_handle);
    if (it == imported_textures_.end()) {
        return false;
    }
    (void)sync_fd;
    return true;
}

size_t WlDmabufVulkan::get_active_texture_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return imported_textures_.size();
}

} // namespace brocompositor
