// The one place wlroots 0.18 (a C library) is included from C++.
//
// Hazards handled here:
//   * wlr_layer_shell_v1.h has a member named `namespace`; it is renamed to
//     `namespace_` for the duration of the include (layout is unchanged).
//   * `[static N]` array parameters only appear in wlr_matrix.h / wlr_scene.h,
//     which brocompositor never includes (no wlr_renderer, no wlr_scene).
//   * The wlroots headers include generated protocol headers
//     ("xdg-shell-protocol.h", ...), produced by CMake with wayland-scanner.
#pragma once

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE 1
#endif

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <pixman.h>

extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/backend/drm.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
}

#include <cstddef>
#include <functional>
#include <utility>

namespace brocompositor::wl {

// A wl_listener that calls a C++ callable. Must not move once connected;
// disconnects itself on destruction.
class Listener {
public:
    Listener() {
        hook_.self = this;
        wl_list_init(&hook_.raw.link);
    }
    ~Listener() { disconnect(); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    void connect(wl_signal* signal, std::function<void(void*)> fn) {
        disconnect();
        fn_ = std::move(fn);
        hook_.raw.notify = &Listener::thunk;
        wl_signal_add(signal, &hook_.raw);
    }
    void disconnect() {
        wl_list_remove(&hook_.raw.link);
        wl_list_init(&hook_.raw.link);
    }
    bool connected() const { return !wl_list_empty(&hook_.raw.link); }

private:
    struct Hook {  // standard layout: raw is at offset 0
        wl_listener raw;
        Listener* self;
    };
    static void thunk(wl_listener* l, void* data) {
        Listener* self = reinterpret_cast<Hook*>(l)->self;
        // Copy: the callback may destroy this listener.
        auto fn = self->fn_;
        fn(data);
    }
    Hook hook_{};
    std::function<void(void*)> fn_;
};

}  // namespace brocompositor::wl
