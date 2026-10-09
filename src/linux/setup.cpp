// Server bring-up and teardown, all on the server thread: display, backend
// (headless / nested / DRM+libinput), render node, protocol globals.
#include "linux/client_surface.h"
#include "linux/drm_util.h"
#include "linux/server_impl.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>

namespace brocompositor::wl {

void init_output_manager(Server* s);

namespace {

void init_logging(int level) {
    static std::once_flag once;
    std::call_once(once, [&] {
        if (const char* env = std::getenv("BROCOMPOSITOR_WLR_LOG")) level = std::atoi(env);
        wlr_log_importance imp = WLR_SILENT;
        if (level >= 3) imp = WLR_DEBUG;
        else if (level == 2) imp = WLR_INFO;
        else if (level == 1) imp = WLR_ERROR;
        wlr_log_init(imp, nullptr);
    });
}

bool check_dmabuf(wlr_dmabuf_attributes* a, void* data) {
    auto* s = static_cast<Server*>(data);
    return wlr_drm_format_set_has(&s->dmabuf_formats, a->format, a->modifier);
}

}  // namespace

bool Server::init_backend(std::string* error) {
    init_logging(config.wlr_log_level);
    display = wl_display_create();
    if (!display) {
        *error = "wl_display_create failed";
        return false;
    }
    loop = wl_display_get_event_loop(display);

    switch (config.backend) {
        case BackendKind::Headless: backend = wlr_headless_backend_create(loop); break;
        case BackendKind::Wayland: backend = wlr_wl_backend_create(loop, nullptr); break;
        case BackendKind::X11: backend = wlr_x11_backend_create(loop, nullptr); break;
        case BackendKind::Drm: {
            session = wlr_session_create(loop);
            if (!session) {
                *error = "cannot create a session (libseat: seatd/logind, or LIBSEAT_BACKEND=noop as root)";
                return false;
            }
            wlr_device* gpus[8];
            ssize_t n = wlr_session_find_gpus(session, 8, gpus);
            if (n <= 0) {
                *error = "no DRM device found";
                return false;
            }
            backend = wlr_multi_backend_create(loop);
            wlr_backend* drm = wlr_drm_backend_create(session, gpus[0], nullptr);
            if (!drm || !wlr_multi_backend_add(backend, drm)) {
                *error = "cannot create the DRM backend";
                return false;
            }
            if (wlr_backend* li = wlr_libinput_backend_create(session)) wlr_multi_backend_add(backend, li);
            break;
        }
        case BackendKind::Auto: backend = wlr_backend_autocreate(loop, &session); break;
    }
    if (!backend) {
        *error = "cannot create the wlroots backend";
        return false;
    }

    // Render node: linux-dmabuf main device and GBM output images.
    int backend_fd = wlr_backend_get_drm_fd(backend);
    render_node_path = config.render_node;
    if (render_node_path.empty()) render_node_path = render_node_of_fd(backend_fd);
    if (render_node_path.empty()) render_node_path = find_render_node();
    if (!render_node_path.empty()) {
        render_fd = open(render_node_path.c_str(), O_RDWR | O_CLOEXEC);
        if (render_fd >= 0) {
            adapter.drm_render_node = dev_of_fd(render_fd);
            gbm = gbm_create_device(render_fd);
        }
    }
    if (backend_fd >= 0 && !wlr_backend_is_headless(backend)) backend_gbm = gbm_create_device(backend_fd);
    return true;
}

void Server::init_dmabuf() {
    if (render_fd < 0) return;
    std::vector<DmabufFormat> fmts = config.dmabuf_formats;
    if (fmts.empty())
        for (uint32_t f : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888})
            fmts.push_back(DmabufFormat{f, {DRM_FORMAT_MOD_LINEAR}});
    for (const auto& f : fmts)
        for (uint64_t m : f.modifiers) wlr_drm_format_set_add(&dmabuf_formats, f.fourcc, m);
    if (dmabuf_formats.len == 0) return;

    wlr_linux_dmabuf_feedback_v1 feedback{};
    feedback.main_device = dev_t(adapter.drm_render_node);
    wl_array_init(&feedback.tranches);
    wlr_linux_dmabuf_feedback_v1_tranche* tranche = wlr_linux_dmabuf_feedback_add_tranche(&feedback);
    if (!tranche) return;
    tranche->target_device = feedback.main_device;
    for (size_t i = 0; i < dmabuf_formats.len; ++i) {
        const wlr_drm_format& f = dmabuf_formats.formats[i];
        for (size_t j = 0; j < f.len; ++j) wlr_drm_format_set_add(&tranche->formats, f.format, f.modifiers[j]);
    }
    linux_dmabuf = wlr_linux_dmabuf_v1_create(display, 4, &feedback);
    wlr_linux_dmabuf_feedback_v1_finish(&feedback);
    if (linux_dmabuf) wlr_linux_dmabuf_v1_set_check_dmabuf_callback(linux_dmabuf, check_dmabuf, this);
}

bool Server::init_globals(std::string* error) {
    static const uint32_t shm_formats[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888,
                                           DRM_FORMAT_XBGR8888};
    wlr_shm_create(display, 1, shm_formats, sizeof shm_formats / sizeof shm_formats[0]);
    compositor = wlr_compositor_create(display, 6, nullptr);
    subcompositor = wlr_subcompositor_create(display);
    new_surface.connect(&compositor->events.new_surface,
                        [this](void* data) { on_new_surface(static_cast<wlr_surface*>(data)); });
    init_dmabuf();

    layout = wlr_output_layout_create(display);
    layout_change.connect(&layout->events.change, [this](void*) { mark_outputs_dirty(); });

    xdg_shell = wlr_xdg_shell_create(display, 6);
    new_toplevel.connect(&xdg_shell->events.new_toplevel,
                         [this](void* data) { on_new_toplevel(static_cast<wlr_xdg_toplevel*>(data)); });
    new_popup.connect(&xdg_shell->events.new_popup,
                      [this](void* data) { on_new_popup(static_cast<wlr_xdg_popup*>(data)); });
    layer_shell = wlr_layer_shell_v1_create(display, 4);
    new_layer_surface.connect(&layer_shell->events.new_surface, [this](void* data) {
        on_new_layer_surface(static_cast<wlr_layer_surface_v1*>(data));
    });
    decoration_manager = wlr_xdg_decoration_manager_v1_create(display);
    new_decoration.connect(&decoration_manager->events.new_toplevel_decoration, [this](void* data) {
        on_new_decoration(static_cast<wlr_xdg_toplevel_decoration_v1*>(data));
    });
    wlr_viewporter_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    presentation = wlr_presentation_create(display, backend);
    activation = wlr_xdg_activation_v1_create(display);
    request_activate.connect(&activation->events.request_activate, [this](void* data) {
        on_activation_request(static_cast<wlr_xdg_activation_v1_request_activate_event*>(data));
    });

    init_seat();
    init_selection();
    init_output_manager(this);
    init_foreign_toplevel();
    init_touch_tablet();
    init_pointer_extras();
    init_idle();
    init_text_input();
    init_session_lock();
    init_screencopy();
    init_image_copy_capture();
    init_gamma();
    init_toplevel_icon();
    init_frame_keepalive();
    new_output.connect(&backend->events.new_output,
                       [this](void* data) { on_new_output(static_cast<wlr_output*>(data)); });

    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (!rt) {
        *error = "XDG_RUNTIME_DIR is not set";
        return false;
    }
    runtime_dir = rt;
    if (config.socket_name.empty()) {
        const char* s = wl_display_add_socket_auto(display);
        if (!s) {
            *error = "wl_display_add_socket_auto failed";
            return false;
        }
        socket = s;
    } else {
        if (wl_display_add_socket(display, config.socket_name.c_str()) != 0) {
            *error = "cannot listen on " + runtime_dir + "/" + config.socket_name;
            return false;
        }
        socket = config.socket_name;
    }
    return true;
}

bool Server::init(std::string* error) {
    if (!init_backend(error) || !init_globals(error) || !init_xwayland(error)) return false;
    wl_event_loop_add_fd(
        loop, dispatcher->fd(), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            static_cast<Server*>(data)->dispatcher->run_pending();
            return 0;
        },
        this);
    if (!wlr_backend_start(backend)) {
        *error = "wlr_backend_start failed";
        return false;
    }
    bool nested = wlr_backend_is_headless(backend) || wlr_backend_is_wl(backend) || wlr_backend_is_x11(backend);
    if (nested) {
        // Headless starts with no outputs; nested backends start with none
        // unless the parent asked. Create the configured ones.
        uint32_t have = uint32_t(outputs.size());
        for (uint32_t i = have; i < config.initial_outputs; ++i) add_output(config.initial_output_size);
    }
    publish_outputs();
    return true;
}

void Server::shutdown() {
    // Detach every listener this server owns before wlroots objects die.
    for (Listener* l : {&new_output, &new_input, &new_surface, &new_toplevel, &new_popup, &new_layer_surface,
                        &new_decoration, &request_activate, &request_cursor_shape, &seat_request_cursor,
                        &seat_request_selection, &seat_request_primary, &seat_request_drag, &seat_start_drag,
                        &output_mgr_apply, &output_mgr_test, &layout_change, &selection_changed, &primary_changed,
                        &drag_icon_destroy, &new_constraint, &new_virtual_keyboard, &new_virtual_pointer,
                        &new_shortcuts_inhibitor, &new_idle_inhibitor, &new_text_input, &new_input_method,
                        &im_commit, &im_new_popup, &im_grab_keyboard, &im_destroy, &im_grab_destroy, &new_lock,
                        &lock_new_surface, &lock_unlock, &lock_destroy, &pointer_grab_begin, &keyboard_grab_begin,
                        &touch_grab_begin})
        l->disconnect();
    for (wl_event_source** src : {&tree_idle, &outputs_idle, &idle_check, &grab_end_idle, &frame_keepalive_timer})
        if (*src) {
            wl_event_source_remove(*src);
            *src = nullptr;
        }

    // Desktop-session state first: its records listen on objects below.
    shutdown_xwayland();
    shutdown_captures();
    shutdown_touch_tablet();
    constraints.clear();
    active_constraint = nullptr;
    inhibitors.clear();
    active_inhibitor = nullptr;
    idle_inhibitors.clear();
    text_inputs.clear();
    input_popups.clear();
    input_method = nullptr;
    text_focus = nullptr;
    lock_surfaces.clear();
    lock = nullptr;
    for (auto& [k, t] : toplevels) foreign_unmap(t->foreign);

    for (auto& [s, r] : surfaces) r->source->shutdown();
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.surfaces.clear();
        mirror.windows.clear();
        mirror.layers.clear();
    }
    for (auto& [id, out] : outputs) free_output_images(*out);
    toplevels.clear();
    popups.clear();
    layers.clear();
    outputs.clear();
    keyboards.clear();
    pointers.clear();
    surfaces.clear();

    if (vkeyboard) {
        wlr_keyboard_finish(vkeyboard);
        delete vkeyboard;
        vkeyboard = nullptr;
    }
    if (vpointer) {
        wlr_pointer_finish(vpointer);
        delete vpointer;
        vpointer = nullptr;
    }
    if (display) wl_display_destroy_clients(display);
    if (backend) wlr_backend_destroy(backend);
    backend = nullptr;
    if (session) wlr_session_destroy(session);
    session = nullptr;
    if (display) wl_display_destroy(display);
    display = nullptr;
    if (backend_gbm) gbm_device_destroy(backend_gbm);
    if (gbm) gbm_device_destroy(gbm);
    backend_gbm = gbm = nullptr;
    if (render_fd >= 0) ::close(render_fd);
    render_fd = -1;
    wlr_drm_format_set_finish(&dmabuf_formats);
}

}  // namespace brocompositor::wl
