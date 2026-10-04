#pragma once

#include "wl_types.h"
#include <unordered_map>
#include <mutex>
#include <vector>
#include <memory>

namespace brocompositor {

class WlBackend;

class WlDmabufVulkan {
public:
    WlDmabufVulkan();
    ~WlDmabufVulkan();

    WlDmabufVulkan(const WlDmabufVulkan&) = delete;
    WlDmabufVulkan& operator=(const WlDmabufVulkan&) = delete;

    bool initialize(WlBackend& backend);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Format queries
    bool is_format_supported(uint32_t drm_format) const;
    std::vector<uint32_t> get_supported_formats() const;
    std::vector<uint64_t> get_modifiers_for_format(uint32_t drm_format) const;

    // Zero-copy GPU buffer import using VK_KHR_external_memory_fd
    std::shared_ptr<VulkanImportedTexture> import_dmabuf(const DmaBufAttributes& attribs);
    bool release_texture(uint64_t texture_handle);
    std::shared_ptr<VulkanImportedTexture> get_texture(uint64_t texture_handle) const;

    // GPU-GPU Synchronization via sync_file FD
    bool import_sync_fence(int sync_fd, uint64_t texture_handle);

    // Stats
    size_t get_active_texture_count() const;

private:
    bool initialized_ = false;
    wlr_linux_dmabuf_v1* dmabuf_v1_ = nullptr;

    mutable std::mutex mutex_;
    uint64_t next_texture_handle_ = 0x50000;
    std::unordered_map<uint64_t, std::shared_ptr<VulkanImportedTexture>> imported_textures_;
    std::vector<uint32_t> supported_formats_;
};

} // namespace brocompositor
