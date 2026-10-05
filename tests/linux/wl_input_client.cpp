// bc_wl_input_client: one xdg_toplevel filled with a colour, printing what
// the newer input protocols deliver to it, one line per event:
//
//   ready / configure <w> <h> / close
//   kbenter / kbleave / key <code> <state> / enter <x> <y> / leave / motion <x> <y> / button <b> <s>
//   tdown <id> <x> <y> / tmotion <id> <x> <y> / tup <id> / tcancel        wl_touch
//   tool_in / tool_out / tool_motion <x> <y> / tool_pressure <p*1000>
//   tool_down / tool_up / tool_button <b> <s> / pad_enter / pad_button <b> <s>   tablet-v2
//   locked / unlocked / confined / unconfined / rel <dx> <dy>     pointer constraints + relative
//   tienter / tileave / preedit <text> / commit <text> / delete <b> <a> / tidone   text-input-v3
//   inhibit_active / inhibit_inactive                             keyboard-shortcuts-inhibit
//   popup_mapped / popup_done                                     xdg_popup (grabbing)
//   token <t>                                                     xdg-activation token
//
// Options: --app-id A --title T --size WxH --color AARRGGBB
//          --lock-pointer / --confine-pointer  --relative  --text-input
//          --inhibit-shortcuts  --idle-inhibit  --activate-self
// stdin:   grabpopup (an xdg_popup with a grab, using the last input serial)
//          activate (xdg-activation token for this surface, then activate it)
#include "linux/wl_client_buffers.h"
#include "linux/wl_client_common.h"

#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "tablet-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

using bctest::out;

namespace {

struct App {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    xdg_wm_base* wm = nullptr;
    zwp_tablet_manager_v2* tablets = nullptr;
    zwp_pointer_constraints_v1* constraints = nullptr;
    zwp_relative_pointer_manager_v1* relative = nullptr;
    zwp_text_input_manager_v3* ti_mgr = nullptr;
    zwp_text_input_v3* ti = nullptr;
    zwp_keyboard_shortcuts_inhibit_manager_v1* inhibit_mgr = nullptr;
    zwp_idle_inhibit_manager_v1* idle_mgr = nullptr;
    xdg_activation_v1* activation = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xsurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_surface* popup_surface = nullptr;

    std::string title = "bc-input", app_id = "bc-input-client";
    int width = 200, height = 150, pending_w = 0, pending_h = 0;
    uint32_t color = 0xFF3080C0;
    bool lock_pointer = false, confine_pointer = false, want_relative = false, want_ti = false;
    bool want_inhibit = false, want_idle = false, activate_self = false;
    bool configured = false, ready = false, running = true;
    uint32_t last_serial = 0;
    bctest::BufferPool pool;
};

void redraw(App& a) {
    wl_buffer* buf = a.pool.shm(a.shm, a.width, a.height, a.color);
    if (!buf) return;
    wl_surface_attach(a.surface, buf, 0, 0);
    wl_surface_damage_buffer(a.surface, 0, 0, a.width, a.height);
    wl_surface_commit(a.surface);
    if (!a.ready) {
        a.ready = true;
        out("ready");
    }
}

void request_activation(App& a);

// ---------------------------------------------------------------- seat

const wl_keyboard_listener keyboard_listener = {
    [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
    [](void* d, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) {
        static_cast<App*>(d)->last_serial = serial;
        out("kbenter");
    },
    [](void*, wl_keyboard*, uint32_t, wl_surface*) { out("kbleave"); },
    [](void* d, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
        static_cast<App*>(d)->last_serial = serial;
        out("key %u %u", key, state);
    },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, wl_keyboard*, int32_t, int32_t) {},
};

const wl_pointer_listener pointer_listener = {
    [](void* d, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        static_cast<App*>(d)->last_serial = serial;
        out("enter %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, wl_pointer*, uint32_t, wl_surface*) { out("leave"); },
    [](void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        out("motion %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void* d, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
        static_cast<App*>(d)->last_serial = serial;
        out("button %u %u", button, state);
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
    [](void*, wl_pointer*) {},
    [](void*, wl_pointer*, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, int32_t) {},
    [](void*, wl_pointer*, uint32_t, int32_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t) {},
#ifdef WL_POINTER_WARP_SINCE_VERSION
    [](void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {},
#endif
};

const wl_touch_listener touch_listener = {
    [](void* d, wl_touch*, uint32_t serial, uint32_t, wl_surface*, int32_t id, wl_fixed_t x, wl_fixed_t y) {
        static_cast<App*>(d)->last_serial = serial;
        out("tdown %d %d %d", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, wl_touch*, uint32_t, uint32_t, int32_t id) { out("tup %d", id); },
    [](void*, wl_touch*, uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y) {
        out("tmotion %d %d %d", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, wl_touch*) { out("tframe"); },
    [](void*, wl_touch*) { out("tcancel"); },
    [](void*, wl_touch*, int32_t, wl_fixed_t, wl_fixed_t) {},
    [](void*, wl_touch*, int32_t, wl_fixed_t) {},
};

void setup_pointer_extras(App& a);

const wl_seat_listener seat_listener = {
    [](void* d, wl_seat* seat, uint32_t caps) {
        auto& a = *static_cast<App*>(d);
        static bool kb = false, touch = false;
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kb) {
            kb = true;
            wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, d);
        }
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a.pointer) {
            a.pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(a.pointer, &pointer_listener, d);
        }
        if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
            touch = true;
            wl_touch_add_listener(wl_seat_get_touch(seat), &touch_listener, d);
        }
    },
    [](void*, wl_seat*, const char*) {},
};

// ---------------------------------------------------------------- tablet-v2

const zwp_tablet_tool_v2_listener tool_listener = {
    [](void*, zwp_tablet_tool_v2*, uint32_t) {},                      // type
    [](void*, zwp_tablet_tool_v2*, uint32_t, uint32_t) {},            // hardware_serial
    [](void*, zwp_tablet_tool_v2*, uint32_t, uint32_t) {},            // hardware_id_wacom
    [](void*, zwp_tablet_tool_v2*, uint32_t) {},                      // capability
    [](void*, zwp_tablet_tool_v2*) {},                                // done
    [](void*, zwp_tablet_tool_v2*) {},                                // removed
    [](void*, zwp_tablet_tool_v2*, uint32_t, zwp_tablet_v2*, wl_surface*) { out("tool_in"); },
    [](void*, zwp_tablet_tool_v2*) { out("tool_out"); },
    [](void* d, zwp_tablet_tool_v2*, uint32_t serial) {
        static_cast<App*>(d)->last_serial = serial;
        out("tool_down");
    },
    [](void*, zwp_tablet_tool_v2*) { out("tool_up"); },
    [](void*, zwp_tablet_tool_v2*, wl_fixed_t x, wl_fixed_t y) {
        out("tool_motion %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, zwp_tablet_tool_v2*, uint32_t p) { out("tool_pressure %u", (p * 1000 + 32767) / 65535); },
    [](void*, zwp_tablet_tool_v2*, uint32_t) {},                        // distance
    [](void*, zwp_tablet_tool_v2*, wl_fixed_t, wl_fixed_t) {},          // tilt
    [](void*, zwp_tablet_tool_v2*, wl_fixed_t) {},                      // rotation
    [](void*, zwp_tablet_tool_v2*, int32_t) {},                         // slider
    [](void*, zwp_tablet_tool_v2*, wl_fixed_t, int32_t) {},             // wheel
    [](void*, zwp_tablet_tool_v2*, uint32_t, uint32_t b, uint32_t s) { out("tool_button %u %u", b, s); },
    [](void*, zwp_tablet_tool_v2*, uint32_t) {},                        // frame
};

const zwp_tablet_v2_listener tablet_listener = {
    [](void*, zwp_tablet_v2*, const char*) {},
    [](void*, zwp_tablet_v2*, uint32_t, uint32_t) {},
    [](void*, zwp_tablet_v2*, const char*) {},
    [](void*, zwp_tablet_v2*) {},
    [](void*, zwp_tablet_v2*) {},
};

const zwp_tablet_pad_group_v2_listener pad_group_listener = {
    [](void*, zwp_tablet_pad_group_v2*, wl_array*) {},
    [](void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_ring_v2*) {},
    [](void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_strip_v2*) {},
    [](void*, zwp_tablet_pad_group_v2*, uint32_t) {},
    [](void*, zwp_tablet_pad_group_v2*) {},
    [](void*, zwp_tablet_pad_group_v2*, uint32_t, uint32_t, uint32_t) {},
};

const zwp_tablet_pad_v2_listener pad_listener = {
    [](void*, zwp_tablet_pad_v2*, zwp_tablet_pad_group_v2* g) {
        zwp_tablet_pad_group_v2_add_listener(g, &pad_group_listener, nullptr);
    },
    [](void*, zwp_tablet_pad_v2*, const char*) {},
    [](void*, zwp_tablet_pad_v2*, uint32_t) {},
    [](void*, zwp_tablet_pad_v2*) {},
    [](void*, zwp_tablet_pad_v2*, uint32_t, uint32_t b, uint32_t s) { out("pad_button %u %u", b, s); },
    [](void*, zwp_tablet_pad_v2*, uint32_t, zwp_tablet_v2*, wl_surface*) { out("pad_enter"); },
    [](void*, zwp_tablet_pad_v2*, uint32_t, wl_surface*) { out("pad_leave"); },
    [](void*, zwp_tablet_pad_v2*) {},
};

const zwp_tablet_seat_v2_listener tablet_seat_listener = {
    [](void*, zwp_tablet_seat_v2*, zwp_tablet_v2* t) { zwp_tablet_v2_add_listener(t, &tablet_listener, nullptr); },
    [](void* d, zwp_tablet_seat_v2*, zwp_tablet_tool_v2* t) { zwp_tablet_tool_v2_add_listener(t, &tool_listener, d); },
    [](void*, zwp_tablet_seat_v2*, zwp_tablet_pad_v2* p) { zwp_tablet_pad_v2_add_listener(p, &pad_listener, nullptr); },
};

// ---------------------------------------------------------------- pointer constraints / relative

const zwp_locked_pointer_v1_listener locked_listener = {
    [](void*, zwp_locked_pointer_v1*) { out("locked"); },
    [](void*, zwp_locked_pointer_v1*) { out("unlocked"); },
};
const zwp_confined_pointer_v1_listener confined_listener = {
    [](void*, zwp_confined_pointer_v1*) { out("confined"); },
    [](void*, zwp_confined_pointer_v1*) { out("unconfined"); },
};
const zwp_relative_pointer_v1_listener relative_listener = {
    [](void*, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
        out("rel %d %d", wl_fixed_to_int(dx), wl_fixed_to_int(dy));
    },
};

void setup_pointer_extras(App& a) {
    if (!a.pointer) return;
    if (a.constraints && (a.lock_pointer || a.confine_pointer)) {
        if (a.lock_pointer) {
            auto* l = zwp_pointer_constraints_v1_lock_pointer(a.constraints, a.surface, a.pointer, nullptr,
                                                              ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            zwp_locked_pointer_v1_add_listener(l, &locked_listener, &a);
        } else {
            // Confined to the top-left 50x50 of the surface.
            wl_region* r = wl_compositor_create_region(a.compositor);
            wl_region_add(r, 0, 0, 50, 50);
            auto* c = zwp_pointer_constraints_v1_confine_pointer(a.constraints, a.surface, a.pointer, r,
                                                                 ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            wl_region_destroy(r);
            zwp_confined_pointer_v1_add_listener(c, &confined_listener, &a);
        }
    }
    if (a.relative && a.want_relative) {
        auto* rp = zwp_relative_pointer_manager_v1_get_relative_pointer(a.relative, a.pointer);
        zwp_relative_pointer_v1_add_listener(rp, &relative_listener, &a);
    }
}

// ---------------------------------------------------------------- text-input-v3

const zwp_text_input_v3_listener ti_listener = {
    [](void* d, zwp_text_input_v3* ti, wl_surface*) {
        auto& a = *static_cast<App*>(d);
        out("tienter");
        zwp_text_input_v3_enable(ti);
        zwp_text_input_v3_set_surrounding_text(ti, "hello", 5, 5);
        zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                           ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        zwp_text_input_v3_set_cursor_rectangle(ti, 20, 30, 2, 16);
        zwp_text_input_v3_commit(ti);
        (void)a;
    },
    [](void*, zwp_text_input_v3* ti, wl_surface*) {
        out("tileave");
        zwp_text_input_v3_disable(ti);
        zwp_text_input_v3_commit(ti);
    },
    [](void*, zwp_text_input_v3*, const char* text, int32_t, int32_t) { out("preedit %s", text ? text : ""); },
    [](void*, zwp_text_input_v3*, const char* text) { out("commit %s", text ? text : ""); },
    [](void*, zwp_text_input_v3*, uint32_t b, uint32_t af) { out("delete %u %u", b, af); },
    [](void*, zwp_text_input_v3*, uint32_t) { out("tidone"); },
};

const zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
    [](void*, zwp_keyboard_shortcuts_inhibitor_v1*) { out("inhibit_active"); },
    [](void*, zwp_keyboard_shortcuts_inhibitor_v1*) { out("inhibit_inactive"); },
};

// ---------------------------------------------------------------- xdg-activation

const xdg_activation_token_v1_listener token_listener = {
    [](void* d, xdg_activation_token_v1* t, const char* token) {
        auto& a = *static_cast<App*>(d);
        out("token %s", token);
        xdg_activation_v1_activate(a.activation, token, a.surface);
        xdg_activation_token_v1_destroy(t);
    },
};

void request_activation(App& a) {
    if (!a.activation) return out("error no-activation");
    xdg_activation_token_v1* t = xdg_activation_v1_get_activation_token(a.activation);
    xdg_activation_token_v1_add_listener(t, &token_listener, &a);
    xdg_activation_token_v1_set_surface(t, a.surface);
    xdg_activation_token_v1_set_app_id(t, a.app_id.c_str());
    if (a.last_serial) xdg_activation_token_v1_set_serial(t, a.last_serial, a.seat);
    xdg_activation_token_v1_commit(t);
}

// ---------------------------------------------------------------- xdg shell

const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); },
};

const xdg_surface_listener xsurface_listener = {
    [](void* d, xdg_surface* xs, uint32_t serial) {
        auto& a = *static_cast<App*>(d);
        xdg_surface_ack_configure(xs, serial);
        if (a.pending_w > 0 && a.pending_h > 0) {
            a.width = a.pending_w;
            a.height = a.pending_h;
        }
        bool first = !a.configured;
        a.configured = true;
        redraw(a);
        if (first && a.activate_self) request_activation(a);
    },
};

const xdg_toplevel_listener toplevel_listener = {
    [](void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        auto& a = *static_cast<App*>(d);
        a.pending_w = w;
        a.pending_h = h;
        out("configure %d %d", w, h);
    },
    [](void* d, xdg_toplevel*) {
        out("close");
        static_cast<App*>(d)->running = false;
    },
    [](void*, xdg_toplevel*, int32_t, int32_t) {},
    [](void*, xdg_toplevel*, wl_array*) {},
};

const xdg_popup_listener popup_listener = {
    [](void*, xdg_popup*, int32_t, int32_t, int32_t, int32_t) {},
    [](void*, xdg_popup*) { out("popup_done"); },
    [](void*, xdg_popup*, uint32_t) {},
};

const xdg_surface_listener popup_xsurface_listener = {
    [](void* d, xdg_surface* xs, uint32_t serial) {
        auto& a = *static_cast<App*>(d);
        xdg_surface_ack_configure(xs, serial);
        wl_buffer* b = a.pool.shm(a.shm, 40, 30, 0xFF00FFFF);
        wl_surface_attach(a.popup_surface, b, 0, 0);
        wl_surface_damage_buffer(a.popup_surface, 0, 0, 40, 30);
        wl_surface_commit(a.popup_surface);
        out("popup_mapped");
    },
};

void grab_popup(App& a) {
    xdg_positioner* pos = xdg_wm_base_create_positioner(a.wm);
    xdg_positioner_set_size(pos, 40, 30);
    xdg_positioner_set_anchor_rect(pos, 10, 10, 1, 1);
    a.popup_surface = wl_compositor_create_surface(a.compositor);
    xdg_surface* xs = xdg_wm_base_get_xdg_surface(a.wm, a.popup_surface);
    xdg_surface_add_listener(xs, &popup_xsurface_listener, &a);
    xdg_popup* p = xdg_surface_get_popup(xs, a.xsurface, pos);
    xdg_popup_add_listener(p, &popup_listener, &a);
    xdg_popup_grab(p, a.seat, a.last_serial);
    xdg_positioner_destroy(pos);
    wl_surface_commit(a.popup_surface);
}

const wl_registry_listener registry_listener = {
    [](void* d, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
        auto& a = *static_cast<App*>(d);
        auto is = [&](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
        auto bind = [&](const wl_interface& i, uint32_t v) {
            return wl_registry_bind(r, name, &i, std::min(version, v));
        };
        if (is(wl_compositor_interface)) a.compositor = static_cast<wl_compositor*>(bind(wl_compositor_interface, 4));
        else if (is(wl_shm_interface)) a.shm = static_cast<wl_shm*>(bind(wl_shm_interface, 1));
        else if (is(wl_seat_interface)) {
            a.seat = static_cast<wl_seat*>(bind(wl_seat_interface, 5));
            wl_seat_add_listener(a.seat, &seat_listener, &a);
        } else if (is(xdg_wm_base_interface)) {
            a.wm = static_cast<xdg_wm_base*>(bind(xdg_wm_base_interface, 2));
            xdg_wm_base_add_listener(a.wm, &wm_listener, &a);
        } else if (is(zwp_tablet_manager_v2_interface))
            a.tablets = static_cast<zwp_tablet_manager_v2*>(bind(zwp_tablet_manager_v2_interface, 1));
        else if (is(zwp_pointer_constraints_v1_interface))
            a.constraints = static_cast<zwp_pointer_constraints_v1*>(bind(zwp_pointer_constraints_v1_interface, 1));
        else if (is(zwp_relative_pointer_manager_v1_interface))
            a.relative =
                static_cast<zwp_relative_pointer_manager_v1*>(bind(zwp_relative_pointer_manager_v1_interface, 1));
        else if (is(zwp_text_input_manager_v3_interface))
            a.ti_mgr = static_cast<zwp_text_input_manager_v3*>(bind(zwp_text_input_manager_v3_interface, 1));
        else if (is(zwp_keyboard_shortcuts_inhibit_manager_v1_interface))
            a.inhibit_mgr = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                bind(zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
        else if (is(zwp_idle_inhibit_manager_v1_interface))
            a.idle_mgr = static_cast<zwp_idle_inhibit_manager_v1*>(bind(zwp_idle_inhibit_manager_v1_interface, 1));
        else if (is(xdg_activation_v1_interface))
            a.activation = static_cast<xdg_activation_v1*>(bind(xdg_activation_v1_interface, 1));
    },
    [](void*, wl_registry*, uint32_t) {},
};

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
        else if (k == "--lock-pointer") a.lock_pointer = true;
        else if (k == "--confine-pointer") a.confine_pointer = true;
        else if (k == "--relative") a.want_relative = true;
        else if (k == "--text-input") a.want_ti = true;
        else if (k == "--inhibit-shortcuts") a.want_inhibit = true;
        else if (k == "--idle-inhibit") a.want_idle = true;
        else if (k == "--activate-self") a.activate_self = true;
    }
    a.display = wl_display_connect(nullptr);
    if (!a.display) {
        out("error connect");
        return 2;
    }
    wl_registry* reg = wl_display_get_registry(a.display);
    wl_registry_add_listener(reg, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);
    if (!a.compositor || !a.shm || !a.wm || !a.seat) {
        out("error globals");
        return 2;
    }
    if (a.tablets) {
        auto* ts = zwp_tablet_manager_v2_get_tablet_seat(a.tablets, a.seat);
        zwp_tablet_seat_v2_add_listener(ts, &tablet_seat_listener, &a);
    }

    a.surface = wl_compositor_create_surface(a.compositor);
    a.xsurface = xdg_wm_base_get_xdg_surface(a.wm, a.surface);
    xdg_surface_add_listener(a.xsurface, &xsurface_listener, &a);
    a.toplevel = xdg_surface_get_toplevel(a.xsurface);
    xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
    xdg_toplevel_set_title(a.toplevel, a.title.c_str());
    xdg_toplevel_set_app_id(a.toplevel, a.app_id.c_str());
    setup_pointer_extras(a);
    if (a.want_ti) {
        if (!a.ti_mgr) return out("error no-text-input"), 2;
        a.ti = zwp_text_input_manager_v3_get_text_input(a.ti_mgr, a.seat);
        zwp_text_input_v3_add_listener(a.ti, &ti_listener, &a);
    }
    if (a.want_inhibit) {
        if (!a.inhibit_mgr) return out("error no-inhibit"), 2;
        auto* in = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(a.inhibit_mgr, a.surface, a.seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(in, &inhibitor_listener, &a);
    }
    if (a.want_idle) {
        if (!a.idle_mgr) return out("error no-idle-inhibit"), 2;
        zwp_idle_inhibit_manager_v1_create_inhibitor(a.idle_mgr, a.surface);
    }
    wl_surface_commit(a.surface);

    int err = bctest::run_client_loop(a.display, a.running, [&](const std::string& line) {
        if (line == "grabpopup") grab_popup(a);
        else if (line == "activate") request_activation(a);
        else if (line == "redraw") redraw(a);
        else if (line == "quit") a.running = false;
    });
    a.pool.clear();
    wl_display_disconnect(a.display);
    return err ? 1 : 0;
}
