// Small Linux helpers shared by the server and CpuMapping: DRM fourcc
// mapping, sync_file export/import on dmabufs, memfd allocation, render
// node discovery.
#pragma once

#include "brocompositor/surface.h"

#include <cstdint>
#include <string>

namespace brocompositor::wl {

PixelFormat pixel_format_of(uint32_t drm_fourcc);
uint32_t bytes_per_pixel(uint32_t drm_fourcc);  // 0 for unknown / multi-planar
bool fourcc_has_alpha(uint32_t drm_fourcc);

// wl_shm format codes equal DRM fourccs except ARGB8888 / XRGB8888 (0 / 1).
uint32_t shm_to_drm(uint32_t shm_format);

// The implicit write fence of a dmabuf as a sync_file (for a reader), or -1
// when the kernel cannot export it or nothing is pending.
int export_sync_file(int dmabuf_fd);
// Attaches a sync_file as a write fence to a dmabuf (implicit sync for the
// next reader, e.g. KMS). Does not take ownership of sync_fd.
bool import_sync_file(int dmabuf_fd, int sync_fd);
// Blocks until a sync_file signals (CPU wait); true when signalled.
bool wait_sync_file(int sync_fd, int timeout_ms);

// memfd of `size` bytes; -1 on failure.
int create_memfd(const char* name, size_t size);

// st_rdev of an open DRM node; 0 on failure.
uint64_t dev_of_fd(int fd);
// First accessible /dev/dri/renderD* ("" when none).
std::string find_render_node();
// Render node path for an open DRM fd (primary or render), "" when none.
std::string render_node_of_fd(int fd);

}  // namespace brocompositor::wl
