// bc_wl_client: a scripted Wayland client for the server tests.
//
// It maps one surface (an xdg_toplevel, or a layer surface with --layer),
// fills it with a solid colour from wl_shm (or a dmabuf with --dmabuf), and
// prints one line per protocol event it receives on stdout, so a test can
// assert on what reached the client:
//
//   ready                      first buffer committed
//   configure <w> <h>          toplevel / layer configure (0 0: client decides)
//   frame <n>                  n-th frame callback
//   presented <n>              wp_presentation feedback "presented"
//   key <code> <0|1>           wl_keyboard.key
//   mods <dep> <latch> <lock>  wl_keyboard.modifiers
//   kbenter / kbleave          keyboard focus
//   enter <x> <y> / leave      pointer focus (surface-local)
//   motion <x> <y>
//   button <code> <0|1>
//   axis <axis> <value>
//   scale <n> / fscale <n120>  preferred buffer scale / fractional scale (x120)
//   output_enter               wl_surface.enter
//   close                      xdg_toplevel.close (the client then exits 0)
//   dmabuf <modifier>          dmabuf buffer allocated (hex modifier)
//
// Options: --title T --app-id A --size WxH --color AARRGGBB --animate
//          --subsurface --popup --layer <top|bottom|overlay|background>
//          --anchor <bits> --exclusive N --keyboard <0|1|2> --dmabuf
//          --udmabuf (dmabuf from /dev/udmabuf, LINEAR, no GBM)
//          --viewport WxH / --crop x,y,w,h (wp_viewporter destination / source)
//          --copy TEXT (wl_data_device selection on keyboard focus: "copied",
//          "cancelled") / --paste (prints "paste <text>" per selection)
//          --csd (request client-side decorations; "decoration server|client")
#include "linux/wl_client_buffers.h"

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"
#define namespace namespace_  // a request argument is named `namespace`
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct App {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wp_viewporter* viewporter = nullptr;
    wl_data_device_manager* ddm = nullptr;
    wl_data_device* data_device = nullptr;
    std::string copy_text;  // --copy: set the selection on keyboard focus
    bool paste = false;     // --paste: print each selection's text/plain
    bool copied = false;
    bool csd = false;  // --csd: ask xdg-decoration for client-side
    int vp_w = 0, vp_h = 0;                  // --viewport destination
    double crop[4] = {-1, -1, -1, -1};       // --crop source x,y,w,h
    xdg_wm_base* wm = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    wp_presentation* presentation = nullptr;
    wp_fractional_scale_manager_v1* fscale_mgr = nullptr;
    zxdg_decoration_manager_v1* deco_mgr = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    bctest::DmabufFeedback feedback;

    wl_surface* surface = nullptr;
    xdg_surface* xsurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zwlr_layer_surface_v1* layer = nullptr;
    wl_surface* sub_surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wl_surface* popup_surface = nullptr;
    xdg_surface* popup_xsurface = nullptr;
    xdg_popup* popup = nullptr;

    std::string title = "bc-test", app_id = "bc-test-client";
    int width = 200, height = 150;
    int pending_w = 0, pending_h = 0;
    uint32_t color = 0xFFFF0000;
    bool animate = false, want_sub = false, want_popup = false, use_dmabuf = false;
    std::string layer_name;
    uint32_t anchor = 0;
    int exclusive = 0;
    int keyboard = 0;  // layer keyboard interactivity (1 exclusive, 2 on-demand)
    bool configured = false, ready = false, running = true;
    int frames = 0, presented = 0;
    wl_callback* frame_cb = nullptr;
    bctest::BufferPool pool;
};

void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void redraw(App& a);

// ---------------------------------------------------------------- frame / presentation

const wl_callback_listener frame_listener = {
    [](void* data, wl_callback* cb, uint32_t) {
        auto& a = *static_cast<App*>(data);
        wl_callback_destroy(cb);
        a.frame_cb = nullptr;
        out("frame %d", ++a.frames);
        if (a.animate) redraw(a);
    },
};

const wp_presentation_feedback_listener feedback_listener = {
    [](void*, struct wp_presentation_feedback*, wl_output*) {},
    [](void* data, struct wp_presentation_feedback* f, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
       uint32_t) {
        auto& a = *static_cast<App*>(data);
        out("presented %d", ++a.presented);
        wp_presentation_feedback_destroy(f);
    },
    [](void*, struct wp_presentation_feedback* f) { wp_presentation_feedback_destroy(f); },
};

void redraw(App& a) {
    if (a.width <= 0 || a.height <= 0) return;
    wl_buffer* buf = a.use_dmabuf ? a.pool.dmabuf(a.dmabuf, a.feedback, a.width, a.height, a.color)
                                  : a.pool.shm(a.shm, a.width, a.height, a.color);
    if (!buf) {
        out("error no-buffer");
        a.running = false;
        return;
    }
    if (a.use_dmabuf && a.pool.last_modifier_reported != a.pool.last_modifier) {
        a.pool.last_modifier_reported = a.pool.last_modifier;
        out("dmabuf %llx", static_cast<unsigned long long>(a.pool.last_modifier));
    }
    wl_surface_attach(a.surface, buf, 0, 0);
    wl_surface_damage_buffer(a.surface, 0, 0, a.width, a.height);
    // At most one outstanding callback: a second commit before the next
    // frame would otherwise add a second callback firing on every frame.
    if (!a.frame_cb) {
        a.frame_cb = wl_surface_frame(a.surface);
        wl_callback_add_listener(a.frame_cb, &frame_listener, &a);
    }
    if (a.presentation) {
        struct wp_presentation_feedback* f = wp_presentation_feedback(a.presentation, a.surface);
        wp_presentation_feedback_add_listener(f, &feedback_listener, &a);
    }
    wl_surface_commit(a.surface);
    if (!a.ready) {
        a.ready = true;
        out("ready");
    }
}

const zxdg_toplevel_decoration_v1_listener decoration_listener = {
    [](void*, zxdg_toplevel_decoration_v1*, uint32_t mode) {
        out("decoration %s", mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? "server" : "client");
    },
};

// ---------------------------------------------------------------- clipboard (wl_data_device)

const wl_data_source_listener source_listener = {
    [](void*, wl_data_source*, const char*) {},
    [](void* data, wl_data_source*, const char*, int32_t fd) {
        auto& a = *static_cast<App*>(data);
        ssize_t r = write(fd, a.copy_text.data(), a.copy_text.size());
        (void)r;
        close(fd);
    },
    [](void*, wl_data_source* s) {
        out("cancelled");
        wl_data_source_destroy(s);
    },
    [](void*, wl_data_source*) {},
    [](void*, wl_data_source*) {},
    [](void*, wl_data_source*, uint32_t) {},
};

// The selection's offers announce their types before the selection event.
const wl_data_offer_listener offer_listener = {
    [](void*, wl_data_offer* o, const char* mime) {
        if (std::strcmp(mime, "text/plain") == 0) wl_proxy_set_user_data(reinterpret_cast<wl_proxy*>(o), o);
    },
    [](void*, wl_data_offer*, uint32_t) {},
    [](void*, wl_data_offer*, uint32_t) {},
};

const wl_data_device_listener data_device_listener = {
    [](void*, wl_data_device*, wl_data_offer* o) { wl_data_offer_add_listener(o, &offer_listener, nullptr); },
    [](void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {},
    [](void*, wl_data_device*) {},
    [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    [](void*, wl_data_device*) {},
    [](void* data, wl_data_device*, wl_data_offer* o) {
        auto& a = *static_cast<App*>(data);
        if (!o) {
            if (a.paste) out("paste-empty");
            return;
        }
        bool text = wl_proxy_get_user_data(reinterpret_cast<wl_proxy*>(o)) == o;
        if (a.paste && text) {
            int p[2];
            if (pipe(p) == 0) {
                wl_data_offer_receive(o, "text/plain", p[1]);
                close(p[1]);
                wl_display_flush(a.display);
                std::string got;
                char buf[256];
                ssize_t n;
                while ((n = read(p[0], buf, sizeof buf)) > 0) got.append(buf, size_t(n));
                close(p[0]);
                out("paste %s", got.c_str());
            }
        }
        wl_data_offer_destroy(o);
    },
};

void on_keyboard_enter(App& a, uint32_t serial) {
    out("kbenter");
    if (a.copy_text.empty() || a.copied || !a.ddm || !a.data_device) return;
    wl_data_source* s = wl_data_device_manager_create_data_source(a.ddm);
    wl_data_source_add_listener(s, &source_listener, &a);
    wl_data_source_offer(s, "text/plain");
    wl_data_source_offer(s, "text/plain;charset=utf-8");
    wl_data_device_set_selection(a.data_device, s, serial);
    a.copied = true;
    out("copied");
}

// ---------------------------------------------------------------- input

const wl_keyboard_listener keyboard_listener = {
    [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
    [](void* data, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) {
        on_keyboard_enter(*static_cast<App*>(data), serial);
    },
    [](void*, wl_keyboard*, uint32_t, wl_surface*) { out("kbleave"); },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) { out("key %u %u", key, state); },
    [](void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t) {
        out("mods %u %u %u", depressed, latched, locked);
    },
    [](void*, wl_keyboard*, int32_t, int32_t) {},
};

const wl_pointer_listener pointer_listener = {
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        out("enter %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, wl_pointer*, uint32_t, wl_surface*) { out("leave"); },
    [](void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        out("motion %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) { out("button %u %u", button, state); },
    [](void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t v) { out("axis %u %d", axis, wl_fixed_to_int(v)); },
    [](void*, wl_pointer*) {},
    [](void*, wl_pointer*, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, int32_t) {},
    [](void*, wl_pointer*, uint32_t, int32_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t) {},
#ifdef WL_POINTER_WARP_SINCE_VERSION  // wayland >= 1.26
    [](void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {},
#endif
};

const wl_seat_listener seat_listener = {
    [](void* data, wl_seat* seat, uint32_t caps) {
        static bool kb = false, ptr = false;
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kb) {
            kb = true;
            wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, data);
        }
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
            ptr = true;
            wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, nullptr);
        }
    },
    [](void*, wl_seat*, const char*) {},
};

// ---------------------------------------------------------------- surfaces

const wl_surface_listener surface_listener = {
    [](void*, wl_surface*, wl_output*) { out("output_enter"); },
    [](void*, wl_surface*, wl_output*) {},
    [](void*, wl_surface*, int32_t factor) { out("scale %d", factor); },
    [](void*, wl_surface*, uint32_t) {},
};

const wp_fractional_scale_v1_listener fscale_listener = {
    [](void*, wp_fractional_scale_v1*, uint32_t scale) { out("fscale %u", scale); },
};

const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); },
};

void make_popup(App& a);
void make_subsurface(App& a);

const xdg_surface_listener xsurface_listener = {
    [](void* data, xdg_surface* xs, uint32_t serial) {
        auto& a = *static_cast<App*>(data);
        xdg_surface_ack_configure(xs, serial);
        if (a.pending_w > 0 && a.pending_h > 0) {
            a.width = a.pending_w;
            a.height = a.pending_h;
        }
        bool first = !a.configured;
        a.configured = true;
        if (first && a.want_sub) make_subsurface(a);
        redraw(a);
        if (first && a.want_popup) make_popup(a);
    },
};

const xdg_toplevel_listener toplevel_listener = {
    [](void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        auto& a = *static_cast<App*>(data);
        a.pending_w = w;
        a.pending_h = h;
        out("configure %d %d", w, h);
    },
    [](void* data, xdg_toplevel*) {
        out("close");
        static_cast<App*>(data)->running = false;
    },
    [](void*, xdg_toplevel*, int32_t, int32_t) {},
    [](void*, xdg_toplevel*, wl_array*) {},
};

const zwlr_layer_surface_v1_listener layer_listener = {
    [](void* data, zwlr_layer_surface_v1* l, uint32_t serial, uint32_t w, uint32_t h) {
        auto& a = *static_cast<App*>(data);
        zwlr_layer_surface_v1_ack_configure(l, serial);
        out("configure %u %u", w, h);
        if (w) a.width = int(w);
        if (h) a.height = int(h);
        a.configured = true;
        redraw(a);
    },
    [](void* data, zwlr_layer_surface_v1*) {
        out("close");
        static_cast<App*>(data)->running = false;
    },
};

void make_subsurface(App& a) {
    a.sub_surface = wl_compositor_create_surface(a.compositor);
    a.subsurface = wl_subcompositor_get_subsurface(a.subcompositor, a.sub_surface, a.surface);
    wl_subsurface_set_position(a.subsurface, 10, 10);
    wl_subsurface_set_desync(a.subsurface);
    wl_buffer* b = a.pool.shm(a.shm, 20, 20, 0xFF00FF00);
    wl_surface_attach(a.sub_surface, b, 0, 0);
    wl_surface_damage_buffer(a.sub_surface, 0, 0, 20, 20);
    wl_surface_commit(a.sub_surface);
}

const xdg_popup_listener popup_listener = {
    [](void*, xdg_popup*, int32_t x, int32_t y, int32_t w, int32_t h) { out("popup_configure %d %d %d %d", x, y, w, h); },
    [](void*, xdg_popup*) { out("popup_done"); },
    [](void*, xdg_popup*, uint32_t) {},
};

const xdg_surface_listener popup_xsurface_listener = {
    [](void* data, xdg_surface* xs, uint32_t serial) {
        auto& a = *static_cast<App*>(data);
        xdg_surface_ack_configure(xs, serial);
        wl_buffer* b = a.pool.shm(a.shm, 40, 30, 0xFF0000FF);
        wl_surface_attach(a.popup_surface, b, 0, 0);
        wl_surface_damage_buffer(a.popup_surface, 0, 0, 40, 30);
        wl_surface_commit(a.popup_surface);
        out("popup_mapped");
    },
};

void make_popup(App& a) {
    xdg_positioner* pos = xdg_wm_base_create_positioner(a.wm);
    xdg_positioner_set_size(pos, 40, 30);
    xdg_positioner_set_anchor_rect(pos, 50, 60, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    a.popup_surface = wl_compositor_create_surface(a.compositor);
    a.popup_xsurface = xdg_wm_base_get_xdg_surface(a.wm, a.popup_surface);
    xdg_surface_add_listener(a.popup_xsurface, &popup_xsurface_listener, &a);
    a.popup = xdg_surface_get_popup(a.popup_xsurface, a.xsurface, pos);
    xdg_popup_add_listener(a.popup, &popup_listener, &a);
    xdg_positioner_destroy(pos);
    wl_surface_commit(a.popup_surface);
}

// ---------------------------------------------------------------- registry

const wl_registry_listener registry_listener = {
    [](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
        auto& a = *static_cast<App*>(data);
        auto is = [&](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
        auto bind = [&](const wl_interface& i, uint32_t v) { return wl_registry_bind(r, name, &i, std::min(version, v)); };
        if (is(wl_compositor_interface)) a.compositor = static_cast<wl_compositor*>(bind(wl_compositor_interface, 6));
        else if (is(wl_subcompositor_interface)) a.subcompositor = static_cast<wl_subcompositor*>(bind(wl_subcompositor_interface, 1));
        else if (is(wl_shm_interface)) a.shm = static_cast<wl_shm*>(bind(wl_shm_interface, 1));
        else if (is(wl_output_interface)) bind(wl_output_interface, 4);  // so wl_surface.enter can name it
        else if (is(wp_viewporter_interface)) a.viewporter = static_cast<wp_viewporter*>(bind(wp_viewporter_interface, 1));
        else if (is(wl_data_device_manager_interface))
            a.ddm = static_cast<wl_data_device_manager*>(bind(wl_data_device_manager_interface, 3));
        else if (is(wl_seat_interface)) {
            a.seat = static_cast<wl_seat*>(bind(wl_seat_interface, 5));
            wl_seat_add_listener(a.seat, &seat_listener, &a);
        } else if (is(xdg_wm_base_interface)) {
            a.wm = static_cast<xdg_wm_base*>(bind(xdg_wm_base_interface, 6));
            xdg_wm_base_add_listener(a.wm, &wm_listener, &a);
        } else if (is(zwlr_layer_shell_v1_interface)) a.layer_shell = static_cast<zwlr_layer_shell_v1*>(bind(zwlr_layer_shell_v1_interface, 4));
        else if (is(wp_presentation_interface)) a.presentation = static_cast<wp_presentation*>(bind(wp_presentation_interface, 1));
        else if (is(wp_fractional_scale_manager_v1_interface)) a.fscale_mgr = static_cast<wp_fractional_scale_manager_v1*>(bind(wp_fractional_scale_manager_v1_interface, 1));
        else if (is(zxdg_decoration_manager_v1_interface)) a.deco_mgr = static_cast<zxdg_decoration_manager_v1*>(bind(zxdg_decoration_manager_v1_interface, 1));
        else if (is(zwp_linux_dmabuf_v1_interface) && version >= 4) {
            a.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(bind(zwp_linux_dmabuf_v1_interface, 4));
        }
    },
    [](void*, wl_registry*, uint32_t) {},
};

uint32_t parse_layer(const std::string& s) {
    if (s == "background") return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    if (s == "bottom") return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
    if (s == "overlay") return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

}  // namespace

int main(int argc, char** argv) {
    App a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--title") a.title = next();
        else if (k == "--app-id") a.app_id = next();
        else if (k == "--size") std::sscanf(next().c_str(), "%dx%d", &a.width, &a.height);
        else if (k == "--color") a.color = uint32_t(std::strtoul(next().c_str(), nullptr, 16));
        else if (k == "--animate") a.animate = true;
        else if (k == "--subsurface") a.want_sub = true;
        else if (k == "--popup") a.want_popup = true;
        else if (k == "--layer") a.layer_name = next();
        else if (k == "--anchor") a.anchor = uint32_t(std::atoi(next().c_str()));
        else if (k == "--exclusive") a.exclusive = std::atoi(next().c_str());
        else if (k == "--keyboard") a.keyboard = std::atoi(next().c_str());
        else if (k == "--viewport") std::sscanf(next().c_str(), "%dx%d", &a.vp_w, &a.vp_h);
        else if (k == "--crop")
            std::sscanf(next().c_str(), "%lf,%lf,%lf,%lf", &a.crop[0], &a.crop[1], &a.crop[2], &a.crop[3]);
        else if (k == "--copy") a.copy_text = next();
        else if (k == "--paste") a.paste = true;
        else if (k == "--csd") a.csd = true;
        else if (k == "--dmabuf") a.use_dmabuf = true;
        else if (k == "--udmabuf") a.use_dmabuf = a.pool.force_udmabuf = true;
    }
    a.display = wl_display_connect(nullptr);
    if (!a.display) {
        out("error connect");
        return 2;
    }
    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);
    if (!a.compositor || !a.shm || (!a.wm && a.layer_name.empty())) {
        out("error globals");
        return 2;
    }
    if (a.use_dmabuf) {
        if (!a.dmabuf || !a.feedback.fetch(a.display, a.dmabuf)) {
            out("error no-dmabuf");
            return 3;
        }
    }

    if (a.ddm && a.seat && (a.paste || !a.copy_text.empty())) {
        a.data_device = wl_data_device_manager_get_data_device(a.ddm, a.seat);
        wl_data_device_add_listener(a.data_device, &data_device_listener, &a);
    }

    a.surface = wl_compositor_create_surface(a.compositor);
    wl_surface_add_listener(a.surface, &surface_listener, &a);
    if (a.viewporter && (a.vp_w > 0 || a.crop[2] > 0)) {
        wp_viewport* vp = wp_viewporter_get_viewport(a.viewporter, a.surface);
        if (a.vp_w > 0) wp_viewport_set_destination(vp, a.vp_w, a.vp_h);
        if (a.crop[2] > 0)
            wp_viewport_set_source(vp, wl_fixed_from_double(a.crop[0]), wl_fixed_from_double(a.crop[1]),
                                   wl_fixed_from_double(a.crop[2]), wl_fixed_from_double(a.crop[3]));
    }
    if (a.fscale_mgr) {
        auto* fs = wp_fractional_scale_manager_v1_get_fractional_scale(a.fscale_mgr, a.surface);
        wp_fractional_scale_v1_add_listener(fs, &fscale_listener, &a);
    }
    if (!a.layer_name.empty()) {
        if (!a.layer_shell) {
            out("error no-layer-shell");
            return 2;
        }
        a.layer = zwlr_layer_shell_v1_get_layer_surface(a.layer_shell, a.surface, nullptr, parse_layer(a.layer_name),
                                                        "bc-test");
        zwlr_layer_surface_v1_add_listener(a.layer, &layer_listener, &a);
        zwlr_layer_surface_v1_set_anchor(a.layer, a.anchor);
        bool horizontal = (a.anchor & 3) == 3;  // top+bottom: vertical bar
        uint32_t w = (a.anchor & 12) == 12 ? 0 : uint32_t(a.width);
        uint32_t h = horizontal ? 0 : uint32_t(a.height);
        zwlr_layer_surface_v1_set_size(a.layer, w, h);
        zwlr_layer_surface_v1_set_exclusive_zone(a.layer, a.exclusive);
        if (a.keyboard) zwlr_layer_surface_v1_set_keyboard_interactivity(a.layer, uint32_t(a.keyboard));
    } else {
        a.xsurface = xdg_wm_base_get_xdg_surface(a.wm, a.surface);
        xdg_surface_add_listener(a.xsurface, &xsurface_listener, &a);
        a.toplevel = xdg_surface_get_toplevel(a.xsurface);
        xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
        xdg_toplevel_set_title(a.toplevel, a.title.c_str());
        xdg_toplevel_set_app_id(a.toplevel, a.app_id.c_str());
        if (a.deco_mgr) {
            auto* deco = zxdg_decoration_manager_v1_get_toplevel_decoration(a.deco_mgr, a.toplevel);
            zxdg_toplevel_decoration_v1_add_listener(deco, &decoration_listener, &a);
            if (a.csd) zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        }
    }
    wl_surface_commit(a.surface);

    while (a.running) {
        while (wl_display_prepare_read(a.display) != 0) wl_display_dispatch_pending(a.display);
        wl_display_flush(a.display);
        pollfd fds[2] = {{wl_display_get_fd(a.display), POLLIN, 0}, {0, POLLIN, 0}};
        int r = poll(fds, 2, 1000);
        if (r > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(a.display) < 0) break;
        } else {
            wl_display_cancel_read(a.display);
        }
        if (r > 0 && (fds[0].revents & (POLLERR | POLLHUP))) break;
        if (r > 0 && (fds[1].revents & (POLLHUP | POLLERR))) break;  // test harness went away
        if (r > 0 && (fds[1].revents & POLLIN)) {
            char buf[256];
            ssize_t n = read(0, buf, sizeof buf - 1);
            if (n <= 0) break;
            buf[n] = 0;
            if (std::strstr(buf, "redraw")) redraw(a);
            if (std::strstr(buf, "quit")) break;
            if (std::strncmp(buf, "color ", 6) == 0) {
                a.color = uint32_t(std::strtoul(buf + 6, nullptr, 16));
                redraw(a);
            }
        }
        if (wl_display_dispatch_pending(a.display) < 0) break;
    }
    int err = wl_display_get_error(a.display);
    a.pool.clear();
    wl_display_disconnect(a.display);
    return err ? 1 : 0;
}
