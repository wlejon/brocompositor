#pragma once

#include "brocompositor/types.h"
#include "brocompositor/window.h"
#include "brocompositor/appbar.h"
#include "brocompositor/workspace.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cmath>

#if defined(__linux__)
#if __has_include(<wayland-server-core.h>)
#define BRO_HAS_WAYLAND 1
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#endif
#endif

#ifndef BRO_HAS_WAYLAND
// Cross-platform forward declarations for Wayland & wlroots C types
struct wl_display;
struct wl_event_loop;
struct wl_listener;
struct wl_signal;
struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_compositor;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_output;
struct wlr_output_layout;
struct wlr_xdg_shell;
struct wlr_xdg_surface;
struct wlr_xdg_toplevel;
struct wlr_xdg_popup;
struct wlr_layer_shell_v1;
struct wlr_layer_surface_v1;
struct wlr_linux_dmabuf_v1;
struct wlr_dmabuf_v1_buffer;
struct wlr_dmabuf_attributes;
#endif

namespace brocompositor {

// DRM FourCC format definitions
constexpr uint32_t DRM_FOURCC_ARGB8888 = 0x34325241; // 'A','R','2','4'
constexpr uint32_t DRM_FOURCC_XRGB8888 = 0x34325258; // 'X','R','2','4'
constexpr uint32_t DRM_FOURCC_ABGR8888 = 0x34324241; // 'A','B','2','4'
constexpr uint32_t DRM_FOURCC_XBGR8888 = 0x34324258; // 'X','B','2','4'
constexpr uint32_t DRM_FOURCC_RGBA8888 = 0x34314152; // 'R','A','1','2'
constexpr uint32_t DRM_FOURCC_RGB565   = 0x36314752; // 'R','G','1','6'
constexpr uint32_t DRM_FOURCC_NV12     = 0x3231564e; // 'N','V','1','2'
constexpr uint64_t DRM_MODIFIER_LINEAR = 0ULL;
constexpr uint64_t DRM_MODIFIER_INVALID = ((1ULL << 56) - 1);

// Wayland Seat & Input definitions
enum class SeatCapability : uint32_t {
    None     = 0,
    Pointer  = 1 << 0,
    Keyboard = 1 << 1,
    Touch    = 1 << 2
};

inline constexpr SeatCapability operator|(SeatCapability a, SeatCapability b) {
    return static_cast<SeatCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr SeatCapability operator&(SeatCapability a, SeatCapability b) {
    return static_cast<SeatCapability>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

enum class PointerButton : uint32_t {
    Left   = 0x110, // BTN_LEFT
    Right  = 0x111, // BTN_RIGHT
    Middle = 0x112  // BTN_MIDDLE
};

enum class ButtonState : uint32_t {
    Released = 0,
    Pressed  = 1
};

enum class GestureType {
    SwipeBegin,
    SwipeUpdate,
    SwipeEnd,
    PinchBegin,
    PinchUpdate,
    PinchEnd,
    HoldBegin,
    HoldEnd
};

// Layer Shell Definitions (wlr_layer_shell_v1)
enum class LayerType {
    Background = 0,
    Bottom     = 1,
    Top        = 2,
    Overlay    = 3
};

enum class LayerAnchor : uint32_t {
    None   = 0,
    Top    = 1 << 0,
    Bottom = 1 << 1,
    Left   = 1 << 2,
    Right  = 1 << 3
};

inline constexpr LayerAnchor operator|(LayerAnchor a, LayerAnchor b) {
    return static_cast<LayerAnchor>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr LayerAnchor operator&(LayerAnchor a, LayerAnchor b) {
    return static_cast<LayerAnchor>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline constexpr bool has_anchor(LayerAnchor mask, LayerAnchor flag) {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
}

enum class LayerKeyboardInteractivity {
    None = 0,
    Exclusive = 1,
    OnDemand = 2
};

// Vulkan external memory import definitions
constexpr uint32_t VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5;
constexpr uint32_t VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO = 1000072000;
constexpr uint32_t VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR = 1000074000;
constexpr uint32_t VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR = 1000074001;

constexpr uint32_t VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT = 0x00000100;
constexpr uint32_t VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT = 0x00000200;

struct VkImportMemoryFdInfoKHR {
    uint32_t sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    const void* pNext = nullptr;
    uint32_t handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
};

struct DmaBufPlane {
    int fd = -1;
    uint32_t stride = 0;
    uint32_t offset = 0;
};

struct DmaBufAttributes {
    int32_t width = 0;
    int32_t height = 0;
    uint32_t format = 0;
    uint64_t modifier = DRM_MODIFIER_LINEAR;
    int n_planes = 0;
    DmaBufPlane planes[4];
};

struct VulkanImportedTexture {
    uint64_t image_handle = 0;
    uint64_t memory_handle = 0;
    uint64_t view_handle = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t format = 0;
    uint64_t modifier = DRM_MODIFIER_LINEAR;
    int plane_count = 0;
    int fds[4] = {-1, -1, -1, -1};
    bool is_valid = false;
};

} // namespace brocompositor
