// Taskbars running as clients: wlr-foreign-toplevel-management (list +
// activate / close / (un)maximize / (un)minimize / fullscreen requests) and
// ext-foreign-toplevel-list (list only; its handles also name windows for
// ext-image-copy-capture). Every mapped window of either shell has one
// handle of each; requests become WindowRequests (foreign = true) for the
// host to decide, like the window's own requests.
#include "linux/server_impl.h"

namespace brocompositor::wl {

void Server::init_foreign_toplevel() {
    foreign_manager = wlr_foreign_toplevel_manager_v1_create(display);
    foreign_list = wlr_ext_foreign_toplevel_list_v1_create(display, 1);
}

void Server::foreign_map(std::unique_ptr<ForeignHandles>& h, WindowId id) {
    foreign_unmap(h);
    h = std::make_unique<ForeignHandles>();
    ForeignHandles* f = h.get();
    f->window = id;
    if (foreign_manager) {
        f->wlr = wlr_foreign_toplevel_handle_v1_create(foreign_manager);
        if (f->wlr) {
            f->req_activate.connect(&f->wlr->events.request_activate, [this, f](void*) {
                if (!locked()) push_window_request(f->window, WindowRequestKind::Activate, true);
            });
            f->req_close.connect(&f->wlr->events.request_close,
                                 [this, f](void*) { push_window_request(f->window, WindowRequestKind::Close, true); });
            f->req_maximize.connect(&f->wlr->events.request_maximize, [this, f](void* data) {
                auto* e = static_cast<wlr_foreign_toplevel_handle_v1_maximized_event*>(data);
                push_window_request(f->window, e->maximized ? WindowRequestKind::Maximize : WindowRequestKind::Unmaximize,
                                    true);
            });
            f->req_minimize.connect(&f->wlr->events.request_minimize, [this, f](void* data) {
                auto* e = static_cast<wlr_foreign_toplevel_handle_v1_minimized_event*>(data);
                push_window_request(f->window, e->minimized ? WindowRequestKind::Minimize : WindowRequestKind::Unminimize,
                                    true);
            });
            f->req_fullscreen.connect(&f->wlr->events.request_fullscreen, [this, f](void* data) {
                auto* e = static_cast<wlr_foreign_toplevel_handle_v1_fullscreen_event*>(data);
                WindowRequest r;
                r.window = f->window;
                r.kind = e->fullscreen ? WindowRequestKind::Fullscreen : WindowRequestKind::Unfullscreen;
                r.foreign = true;
                if (OutputRec* o = output_rec(e->output)) r.monitor = o->id;
                server_events.push(r);
            });
            f->wlr_destroy.connect(&f->wlr->events.destroy, [f](void*) {
                f->wlr = nullptr;
                f->req_activate.disconnect();
                f->req_close.disconnect();
                f->req_maximize.disconnect();
                f->req_minimize.disconnect();
                f->req_fullscreen.disconnect();
                f->wlr_destroy.disconnect();
            });
        }
    }
    if (foreign_list) {
        wlr_ext_foreign_toplevel_handle_v1_state st{"", ""};
        f->ext = wlr_ext_foreign_toplevel_handle_v1_create(foreign_list, &st);
    }
}

void Server::foreign_update(ForeignHandles* f, const WindowSnapshot& s, bool activated, bool minimized,
                            const std::set<wlr_output*>& outs) {
    if (!f) return;
    if (f->wlr) {
        if (s.title != f->title) wlr_foreign_toplevel_handle_v1_set_title(f->wlr, s.title.c_str());
        if (s.app_id != f->app_id) wlr_foreign_toplevel_handle_v1_set_app_id(f->wlr, s.app_id.c_str());
        if (s.maximized != f->maximized) wlr_foreign_toplevel_handle_v1_set_maximized(f->wlr, s.maximized);
        if (s.fullscreen != f->fullscreen) wlr_foreign_toplevel_handle_v1_set_fullscreen(f->wlr, s.fullscreen);
        if (minimized != f->minimized) wlr_foreign_toplevel_handle_v1_set_minimized(f->wlr, minimized);
        if (activated != f->activated) wlr_foreign_toplevel_handle_v1_set_activated(f->wlr, activated);
        for (wlr_output* o : f->outputs)
            if (!outs.count(o)) wlr_foreign_toplevel_handle_v1_output_leave(f->wlr, o);
        for (wlr_output* o : outs)
            if (!f->outputs.count(o)) wlr_foreign_toplevel_handle_v1_output_enter(f->wlr, o);
        if (s.owner != f->parent) {
            wlr_foreign_toplevel_handle_v1* parent = nullptr;
            WindowRef p = window_ref(s.owner);
            if (p.xdg && p.xdg->foreign) parent = p.xdg->foreign->wlr;
            else if (p.x && p.x->foreign) parent = p.x->foreign->wlr;
            wlr_foreign_toplevel_handle_v1_set_parent(f->wlr, parent);
        }
    }
    if (f->ext && (s.title != f->title || s.app_id != f->app_id)) {
        wlr_ext_foreign_toplevel_handle_v1_state st{s.title.c_str(), s.app_id.c_str()};
        wlr_ext_foreign_toplevel_handle_v1_update_state(f->ext, &st);
    }
    f->title = s.title;
    f->app_id = s.app_id;
    f->maximized = s.maximized;
    f->fullscreen = s.fullscreen;
    f->minimized = minimized;
    f->activated = activated;
    f->outputs = outs;
    f->parent = s.owner;
}

void Server::foreign_unmap(std::unique_ptr<ForeignHandles>& h) {
    if (!h) return;
    if (h->wlr) {
        wlr_foreign_toplevel_handle_v1* w = h->wlr;
        h->wlr_destroy.disconnect();
        h->req_activate.disconnect();
        h->req_close.disconnect();
        h->req_maximize.disconnect();
        h->req_minimize.disconnect();
        h->req_fullscreen.disconnect();
        h->wlr = nullptr;
        wlr_foreign_toplevel_handle_v1_destroy(w);
    }
    if (h->ext) wlr_ext_foreign_toplevel_handle_v1_destroy(h->ext);
    h.reset();
}

// The window an ext_foreign_toplevel_handle_v1 resource (some client's
// object) stands for.
WindowId Server::window_of_ext_handle(wl_resource* handle_resource) {
    auto match = [&](ForeignHandles* f) {
        if (!f || !f->ext) return false;
        wl_resource* r;
        wl_resource_for_each(r, &f->ext->resources) if (r == handle_resource) return true;
        return false;
    };
    for (auto& [k, t] : toplevels)
        if (match(t->foreign.get())) return t->id;
#ifdef BC_HAVE_XWAYLAND
    for (auto& [k, x] : xsurfaces)
        if (match(x->foreign.get())) return x->id;
#endif
    return kNoWindow;
}

}  // namespace brocompositor::wl
