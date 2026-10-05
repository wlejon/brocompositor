// bc_wl_session_client: the session-level roles for the server tests, one
// per run, printing a line per event:
//
//   --ime            input-method-v2: im_activate / im_deactivate /
//                    im_surrounding <text> / im_key <code> <state> /
//                    im_popup_rect <x> <y> <w> <h> / im_unavailable
//                    stdin: commit <text> | preedit <text> | grab | popup
//   --lock           ext-session-lock-v1 with a surface per output (colour
//                    --color): locked / finished / lock_configure <w> <h> /
//                    key <code> <state> / enter <x> <y>
//                    stdin: unlock (unlocked) | die (exits without unlocking)
//   --toplevels      ext-foreign-toplevel-list: toplevel <app_id> <title> /
//                    toplevel_closed <app_id>
//   --capture output | --capture toplevel:<app_id>
//                    ext-image-copy-capture: session <w> <h> / frame_ready <n>
//                    / frame_failed <reason> / stopped / damage <x> <y> <w> <h>
//                    / pixel <x> <y> <AARRGGBB> for each --pixel x,y;
//                    --frames N captures N frames (each after the last).
#include "linux/wl_client_common.h"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using bctest::out;

namespace {

struct ShmBuffer {
    wl_buffer* buffer = nullptr;
    void* map = nullptr;
    size_t size = 0;
    int w = 0, h = 0, stride = 0;
};

ShmBuffer make_shm(wl_shm* shm, int w, int h, uint32_t format, uint32_t fill) {
    ShmBuffer b;
    b.w = w;
    b.h = h;
    b.stride = w * 4;
    b.size = size_t(b.stride) * size_t(h);
    int fd = memfd_create("bc-session", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(b.size)) != 0) return b;
    b.map = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b.map == MAP_FAILED) {
        b.map = nullptr;
        close(fd);
        return b;
    }
    auto* px = static_cast<uint32_t*>(b.map);
    std::fill(px, px + size_t(w) * size_t(h), fill);
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, int32_t(b.size));
    b.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, b.stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct LockSurface {
    struct App* app = nullptr;
    wl_surface* surface = nullptr;
    ext_session_lock_surface_v1* ls = nullptr;
    ShmBuffer buf;
};

struct App {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    std::vector<wl_output*> outputs;
    zwp_input_method_manager_v2* im_mgr = nullptr;
    ext_session_lock_manager_v1* lock_mgr = nullptr;
    ext_foreign_toplevel_list_v1* toplevel_list = nullptr;
    ext_image_copy_capture_manager_v1* copy_mgr = nullptr;
    ext_output_image_capture_source_manager_v1* output_sources = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_sources = nullptr;
    bool running = true;
    uint32_t color = 0xFF10C010;

    // --ime
    zwp_input_method_v2* im = nullptr;
    uint32_t im_serial = 0;
    bool im_pending_active = false, im_active = false;
    wl_surface* popup_surface = nullptr;
    // --lock
    ext_session_lock_v1* lock = nullptr;
    std::vector<std::unique_ptr<LockSurface>> lock_surfaces;
    // --toplevels / --capture toplevel:<app_id>
    struct Handle {
        App* app = nullptr;
        ext_foreign_toplevel_handle_v1* h = nullptr;
        std::string title, app_id;
        bool announced = false;
    };
    std::vector<std::unique_ptr<Handle>> handles;
    bool print_toplevels = false;
    // --capture
    std::string capture;
    ext_image_copy_capture_session_v1* session = nullptr;
    int sw = 0, sh = 0;
    uint32_t shm_format = ~0u;
    bool session_ready = false;
    ShmBuffer cap;
    int frames_wanted = 1, frames_done = 0;
    std::vector<std::pair<int, int>> pixels;
};

// ---------------------------------------------------------------- input method

const zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, int32_t fd, uint32_t) { close(fd); },
    [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        out("im_key %u %u", key, state);
    },
    [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, zwp_input_method_keyboard_grab_v2*, int32_t, int32_t) {},
};

const zwp_input_popup_surface_v2_listener popup_listener = {
    [](void*, zwp_input_popup_surface_v2*, int32_t x, int32_t y, int32_t w, int32_t h) {
        out("im_popup_rect %d %d %d %d", x, y, w, h);
    },
};

const zwp_input_method_v2_listener im_listener = {
    [](void* d, zwp_input_method_v2*) { static_cast<App*>(d)->im_pending_active = true; },
    [](void* d, zwp_input_method_v2*) { static_cast<App*>(d)->im_pending_active = false; },
    [](void*, zwp_input_method_v2*, const char* text, uint32_t, uint32_t) { out("im_surrounding %s", text); },
    [](void*, zwp_input_method_v2*, uint32_t) {},
    [](void*, zwp_input_method_v2*, uint32_t, uint32_t) {},
    [](void* d, zwp_input_method_v2*) {
        auto& a = *static_cast<App*>(d);
        ++a.im_serial;
        if (a.im_pending_active != a.im_active) {
            a.im_active = a.im_pending_active;
            out(a.im_active ? "im_activate" : "im_deactivate");
        }
    },
    [](void* d, zwp_input_method_v2*) {
        out("im_unavailable");
        static_cast<App*>(d)->running = false;
    },
};

void im_command(App& a, const std::string& line) {
    if (line.rfind("commit ", 0) == 0) {
        zwp_input_method_v2_commit_string(a.im, line.substr(7).c_str());
        zwp_input_method_v2_commit(a.im, a.im_serial);
    } else if (line.rfind("preedit ", 0) == 0) {
        std::string t = line.substr(8);
        zwp_input_method_v2_set_preedit_string(a.im, t.c_str(), 0, int32_t(t.size()));
        zwp_input_method_v2_commit(a.im, a.im_serial);
    } else if (line == "grab") {
        auto* g = zwp_input_method_v2_grab_keyboard(a.im);
        zwp_input_method_keyboard_grab_v2_add_listener(g, &grab_listener, &a);
        out("im_grabbed");
    } else if (line == "popup") {
        a.popup_surface = wl_compositor_create_surface(a.compositor);
        auto* p = zwp_input_method_v2_get_input_popup_surface(a.im, a.popup_surface);
        zwp_input_popup_surface_v2_add_listener(p, &popup_listener, &a);
        ShmBuffer b = make_shm(a.shm, 60, 20, WL_SHM_FORMAT_ARGB8888, 0xFFFFFF00);
        wl_surface_attach(a.popup_surface, b.buffer, 0, 0);
        wl_surface_damage_buffer(a.popup_surface, 0, 0, 60, 20);
        wl_surface_commit(a.popup_surface);
        out("im_popup");
    }
}

// ---------------------------------------------------------------- session lock

const wl_keyboard_listener keyboard_listener = {
    [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
    [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) { out("kbenter"); },
    [](void*, wl_keyboard*, uint32_t, wl_surface*) { out("kbleave"); },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) { out("key %u %u", key, state); },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, wl_keyboard*, int32_t, int32_t) {},
};

const wl_pointer_listener pointer_listener = {
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        out("enter %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
    },
    [](void*, wl_pointer*, uint32_t, wl_surface*) { out("leave"); },
    [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t b, uint32_t s) { out("button %u %u", b, s); },
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

const ext_session_lock_surface_v1_listener lock_surface_listener = {
    [](void* d, ext_session_lock_surface_v1* ls, uint32_t serial, uint32_t w, uint32_t h) {
        auto& s = *static_cast<LockSurface*>(d);
        ext_session_lock_surface_v1_ack_configure(ls, serial);
        out("lock_configure %u %u", w, h);
        if (!s.buf.buffer || s.buf.w != int(w) || s.buf.h != int(h))
            s.buf = make_shm(s.app->shm, int(w), int(h), WL_SHM_FORMAT_XRGB8888, s.app->color);
        wl_surface_attach(s.surface, s.buf.buffer, 0, 0);
        wl_surface_damage_buffer(s.surface, 0, 0, int(w), int(h));
        wl_surface_commit(s.surface);
    },
};

const ext_session_lock_v1_listener lock_listener = {
    [](void*, ext_session_lock_v1*) { out("locked"); },
    [](void* d, ext_session_lock_v1*) {
        out("finished");
        static_cast<App*>(d)->running = false;
    },
};

void start_lock(App& a) {
    a.lock = ext_session_lock_manager_v1_lock(a.lock_mgr);
    ext_session_lock_v1_add_listener(a.lock, &lock_listener, &a);
    for (wl_output* o : a.outputs) {
        auto s = std::make_unique<LockSurface>();
        s->app = &a;
        s->surface = wl_compositor_create_surface(a.compositor);
        s->ls = ext_session_lock_v1_get_lock_surface(a.lock, s->surface, o);
        ext_session_lock_surface_v1_add_listener(s->ls, &lock_surface_listener, s.get());
        a.lock_surfaces.push_back(std::move(s));
    }
}

// ---------------------------------------------------------------- toplevel list

const ext_foreign_toplevel_handle_v1_listener handle_listener = {
    [](void* d, ext_foreign_toplevel_handle_v1*) {
        auto& h = *static_cast<App::Handle*>(d);
        if (h.app->print_toplevels) out("toplevel_closed %s", h.app_id.c_str());
        h.h = nullptr;
    },
    [](void* d, ext_foreign_toplevel_handle_v1*) {
        auto& h = *static_cast<App::Handle*>(d);
        if (h.app->print_toplevels) out("toplevel %s %s", h.app_id.c_str(), h.title.c_str());
        h.announced = true;
    },
    [](void* d, ext_foreign_toplevel_handle_v1*, const char* t) { static_cast<App::Handle*>(d)->title = t; },
    [](void* d, ext_foreign_toplevel_handle_v1*, const char* id) { static_cast<App::Handle*>(d)->app_id = id; },
    [](void*, ext_foreign_toplevel_handle_v1*, const char*) {},
};

const ext_foreign_toplevel_list_v1_listener list_listener = {
    [](void* d, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* h) {
        auto& a = *static_cast<App*>(d);
        auto rec = std::make_unique<App::Handle>();
        rec->app = &a;
        rec->h = h;
        ext_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, rec.get());
        a.handles.push_back(std::move(rec));
    },
    [](void*, ext_foreign_toplevel_list_v1*) {},
};

// ---------------------------------------------------------------- image copy capture

void capture_frame(App& a);

const ext_image_copy_capture_frame_v1_listener frame_listener = {
    [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
    [](void*, ext_image_copy_capture_frame_v1*, int32_t x, int32_t y, int32_t w, int32_t h) {
        out("damage %d %d %d %d", x, y, w, h);
    },
    [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    [](void* d, ext_image_copy_capture_frame_v1* f) {
        auto& a = *static_cast<App*>(d);
        ext_image_copy_capture_frame_v1_destroy(f);
        out("frame_ready %d", ++a.frames_done);
        const auto* px = static_cast<const uint32_t*>(a.cap.map);
        for (auto [x, y] : a.pixels)
            if (px && x >= 0 && y >= 0 && x < a.cap.w && y < a.cap.h)
                out("pixel %d %d %08X", x, y, px[size_t(y) * size_t(a.cap.w) + size_t(x)]);
        if (a.frames_done < a.frames_wanted) capture_frame(a);
    },
    [](void* d, ext_image_copy_capture_frame_v1* f, uint32_t reason) {
        ext_image_copy_capture_frame_v1_destroy(f);
        out("frame_failed %u", reason);
        (void)d;
    },
};

void capture_frame(App& a) {
    if (!a.cap.buffer || a.cap.w != a.sw || a.cap.h != a.sh) {
        if (a.cap.buffer) {
            wl_buffer_destroy(a.cap.buffer);
            munmap(a.cap.map, a.cap.size);
        }
        a.cap = make_shm(a.shm, a.sw, a.sh, a.shm_format, 0);
    }
    auto* f = ext_image_copy_capture_session_v1_create_frame(a.session);
    ext_image_copy_capture_frame_v1_add_listener(f, &frame_listener, &a);
    ext_image_copy_capture_frame_v1_attach_buffer(f, a.cap.buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(f, 0, 0, a.sw, a.sh);
    ext_image_copy_capture_frame_v1_capture(f);
}

const ext_image_copy_capture_session_v1_listener session_listener = {
    [](void* d, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto& a = *static_cast<App*>(d);
        a.sw = int(w);
        a.sh = int(h);
    },
    [](void* d, ext_image_copy_capture_session_v1*, uint32_t f) {
        auto& a = *static_cast<App*>(d);
        if (a.shm_format == ~0u) a.shm_format = f;
    },
    [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
    [](void* d, ext_image_copy_capture_session_v1*) {
        auto& a = *static_cast<App*>(d);
        out("session %d %d", a.sw, a.sh);
        if (!a.session_ready) {
            a.session_ready = true;
            capture_frame(a);
        }
    },
    [](void* d, ext_image_copy_capture_session_v1*) {
        out("stopped");
        static_cast<App*>(d)->running = false;
    },
};

bool start_capture(App& a) {
    if (!a.copy_mgr) return out("error no-image-copy-capture"), false;
    ext_image_capture_source_v1* src = nullptr;
    if (a.capture == "output") {
        if (!a.output_sources || a.outputs.empty()) return out("error no-output-source"), false;
        src = ext_output_image_capture_source_manager_v1_create_source(a.output_sources, a.outputs[0]);
    } else if (a.capture.rfind("toplevel:", 0) == 0) {
        std::string want = a.capture.substr(9);
        if (!a.toplevel_sources) return out("error no-toplevel-source"), false;
        for (auto& h : a.handles)
            if (h->h && h->app_id == want) src = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
                                                 a.toplevel_sources, h->h);
        if (!src) return out("error no-such-toplevel"), false;
    }
    a.session = ext_image_copy_capture_manager_v1_create_session(a.copy_mgr, src, 0);
    ext_image_copy_capture_session_v1_add_listener(a.session, &session_listener, &a);
    ext_image_capture_source_v1_destroy(src);
    return true;
}

// ---------------------------------------------------------------- registry

const wl_seat_listener seat_listener = {
    [](void*, wl_seat* seat, uint32_t caps) {
        static bool kb = false, ptr = false;
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kb) {
            kb = true;
            wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, nullptr);
        }
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
            ptr = true;
            wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, nullptr);
        }
    },
    [](void*, wl_seat*, const char*) {},
};

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
        }
        else if (is(wl_output_interface)) a.outputs.push_back(static_cast<wl_output*>(bind(wl_output_interface, 3)));
        else if (is(zwp_input_method_manager_v2_interface))
            a.im_mgr = static_cast<zwp_input_method_manager_v2*>(bind(zwp_input_method_manager_v2_interface, 1));
        else if (is(ext_session_lock_manager_v1_interface))
            a.lock_mgr = static_cast<ext_session_lock_manager_v1*>(bind(ext_session_lock_manager_v1_interface, 1));
        else if (is(ext_foreign_toplevel_list_v1_interface)) {
            a.toplevel_list =
                static_cast<ext_foreign_toplevel_list_v1*>(bind(ext_foreign_toplevel_list_v1_interface, 1));
            ext_foreign_toplevel_list_v1_add_listener(a.toplevel_list, &list_listener, &a);
        } else if (is(ext_image_copy_capture_manager_v1_interface))
            a.copy_mgr = static_cast<ext_image_copy_capture_manager_v1*>(
                bind(ext_image_copy_capture_manager_v1_interface, 1));
        else if (is(ext_output_image_capture_source_manager_v1_interface))
            a.output_sources = static_cast<ext_output_image_capture_source_manager_v1*>(
                bind(ext_output_image_capture_source_manager_v1_interface, 1));
        else if (is(ext_foreign_toplevel_image_capture_source_manager_v1_interface))
            a.toplevel_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
                bind(ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    },
    [](void*, wl_registry*, uint32_t) {},
};

}  // namespace

int main(int argc, char** argv) {
    App a;
    std::string mode;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--ime" || k == "--lock" || k == "--toplevels") mode = k;
        else if (k == "--capture") mode = k, a.capture = next();
        else if (k == "--color") a.color = uint32_t(std::strtoul(next().c_str(), nullptr, 16));
        else if (k == "--frames") a.frames_wanted = std::atoi(next().c_str());
        else if (k == "--pixel") {
            int x = 0, y = 0;
            if (std::sscanf(next().c_str(), "%d,%d", &x, &y) == 2) a.pixels.emplace_back(x, y);
        }
    }
    a.display = wl_display_connect(nullptr);
    if (!a.display) {
        out("error connect");
        return 2;
    }
    wl_registry* reg = wl_display_get_registry(a.display);
    wl_registry_add_listener(reg, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);  // toplevel handles and their done events
    if (!a.compositor || !a.shm || !a.seat) {
        out("error globals");
        return 2;
    }

    if (mode == "--ime") {
        if (!a.im_mgr) return out("error no-input-method"), 2;
        a.im = zwp_input_method_manager_v2_get_input_method(a.im_mgr, a.seat);
        zwp_input_method_v2_add_listener(a.im, &im_listener, &a);
        out("im_ready");
    } else if (mode == "--lock") {
        if (!a.lock_mgr) return out("error no-session-lock"), 2;
        start_lock(a);
    } else if (mode == "--toplevels") {
        if (!a.toplevel_list) return out("error no-toplevel-list"), 2;
        a.print_toplevels = true;
        for (auto& h : a.handles)
            if (h->h && h->announced) out("toplevel %s %s", h->app_id.c_str(), h->title.c_str());
        out("list_done");
    } else if (mode == "--capture") {
        if (!start_capture(a)) return 2;
    } else {
        out("error mode");
        return 2;
    }

    int err = bctest::run_client_loop(a.display, a.running, [&](const std::string& line) {
        if (mode == "--ime") im_command(a, line);
        if (line == "unlock" && a.lock) {
            ext_session_lock_v1_unlock_and_destroy(a.lock);
            a.lock = nullptr;
            wl_display_roundtrip(a.display);
            out("unlocked");
        }
        if (line == "die") _exit(0);  // drops the lock without unlocking
        if (line == "quit") a.running = false;
    });
    wl_display_disconnect(a.display);
    return err ? 1 : 0;
}
