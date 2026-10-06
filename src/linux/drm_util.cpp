#include "linux/drm_util.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace brocompositor::wl {

PixelFormat pixel_format_of(uint32_t f) {
    switch (f) {
        case DRM_FORMAT_ARGB8888: return PixelFormat::BGRA8Unorm;
        case DRM_FORMAT_XRGB8888: return PixelFormat::BGRX8Unorm;
        case DRM_FORMAT_ABGR8888: return PixelFormat::RGBA8Unorm;
        case DRM_FORMAT_XBGR8888: return PixelFormat::RGBX8Unorm;
        case 0: return PixelFormat::Unknown;
        default: return PixelFormat::Other;
    }
}

uint32_t bytes_per_pixel(uint32_t f) {
    switch (f) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_XBGR2101010: return 4;
        case DRM_FORMAT_RGB565:
        case DRM_FORMAT_BGR565: return 2;
        case DRM_FORMAT_ABGR16161616F:
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_ABGR16161616:
        case DRM_FORMAT_XBGR16161616: return 8;
        default: return 0;
    }
}

bool fourcc_has_alpha(uint32_t f) {
    switch (f) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_XBGR16161616:
        case DRM_FORMAT_RGB565:
        case DRM_FORMAT_BGR565: return false;
        default: return true;
    }
}

uint32_t shm_to_drm(uint32_t shm) {
    if (shm == 0) return DRM_FORMAT_ARGB8888;
    if (shm == 1) return DRM_FORMAT_XRGB8888;
    return shm;
}

int export_sync_file(int dmabuf_fd) {
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
    if (dmabuf_fd < 0) return -1;
    dma_buf_export_sync_file req{};
    req.flags = DMA_BUF_SYNC_READ;
    req.fd = -1;
    if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) != 0) return -1;
    return req.fd;
#else
    (void)dmabuf_fd;
    return -1;
#endif
}

bool import_sync_file(int dmabuf_fd, int sync_fd) {
#ifdef DMA_BUF_IOCTL_IMPORT_SYNC_FILE
    if (dmabuf_fd < 0 || sync_fd < 0) return false;
    dma_buf_import_sync_file req{};
    req.flags = DMA_BUF_SYNC_WRITE;
    req.fd = sync_fd;
    return ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &req) == 0;
#else
    (void)dmabuf_fd;
    (void)sync_fd;
    return false;
#endif
}

bool wait_sync_file(int sync_fd, int timeout_ms) {
    if (sync_fd < 0) return true;
    pollfd p{sync_fd, POLLIN, 0};
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r > 0) return true;
        if (r == 0) return false;
        if (errno != EINTR) return false;
    }
}

int create_memfd(const char* name, size_t size) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, off_t(size)) != 0) {
        ::close(fd);
        return -1;
    }
    // Clients and the host may map it; it never shrinks.
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
    return fd;
}

uint64_t dev_of_fd(int fd) {
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) != 0) return 0;
    return uint64_t(st.st_rdev);
}

std::string find_render_node() {
    for (int i = 128; i < 192; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
        if (access(path, R_OK | W_OK) == 0) return path;
    }
    return {};
}

std::string render_node_of_fd(int fd) {
    if (fd < 0) return {};
    char* name = drmGetRenderDeviceNameFromFd(fd);
    if (!name) {
        if (drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER) {
            char* n = drmGetDeviceNameFromFd2(fd);
            std::string s = n ? n : "";
            free(n);
            return s;
        }
        return {};
    }
    std::string s = name;
    free(name);
    return s;
}  // namespace

}  // namespace brocompositor::wl

#include "linux/drm/drm_output.h"
#include "linux/wlr.h"

namespace brocompositor::wl {

bool is_drm_output(wlr_output* output) {
    if (!output) return false;
    return drm::is_direct_drm_output(output) || wlr_output_is_drm(output);
}

}  // namespace brocompositor::wl
