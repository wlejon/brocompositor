// XWayland: wlroots' X window manager (wlr_xwayland / xwm) maps X11 windows
// into the same model as xdg toplevels. Managed windows become portable
// windows (WM_CLASS class -> app_id, instance -> class_name, title,
// _NET_WM_PID, WM_TRANSIENT_FOR -> owner, _NET_WM_STATE maximize /
// fullscreen); override-redirect windows become unmanaged surfaces drawn
// above every window. The xwm handles ICCCM / EWMH (focus, WM_DELETE_WINDOW,
// _NET_WM_STATE, _NET_WORKAREA) and bridges X selections (CLIPBOARD,
// PRIMARY) with the seat's selections.
//
// Coordinates: the X root window is the layout space, 1:1 (see
// XwaylandMode in server.h for HiDPI).
#include "linux/server_impl.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace brocompositor::wl {

#ifdef BC_HAVE_XWAYLAND

namespace {

// The pid of the X server listening on display `d` (its abstract socket,
// where every Linux X server listens), 0 when none is, -1 when one is but
// its pid cannot be had.
pid_t x_server_pid(int d) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const int n = std::snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "/tmp/.X11-unix/X%d", d);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
    pid_t pid = 0;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0) {
        ucred cred{};
        socklen_t clen = sizeof(cred);
        pid = ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 && cred.pid > 0 ? cred.pid : -1;
    }
    ::close(fd);
    return pid;
}

// wlroots picks Xwayland's display by its lock file (/tmp/.X<n>-lock): the
// first display without one, or with a dead owner, is tried. An X server
// that listens without a lock file — sddm's Xorg, started with -displayfd,
// leaves none — makes that try fail ("Failed to bind socket
// @/tmp/.X11-unix/X0: Address already in use") before wlroots moves on to
// the next display. Each display some live server listens on without a lock
// gets the lock it should have had, naming that server, so wlroots (and any
// other X server) passes over it as taken. A dead owner later makes the lock
// stale, which every X server already cleans up. A lock left stale by an
// earlier server on a display a new one now serves (the lock written here for
// the greeter's Xorg, after sddm restarted it) is replaced the same way:
// wlroots would remove it and fail on the bind just as without one.

// The pid a lock file names; 0 when it is unreadable.
pid_t lock_owner(const char* lock) {
    const int fd = ::open(lock, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char text[16] = {};
    const ssize_t n = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    if (n <= 0) return 0;
    return static_cast<pid_t>(std::strtol(text, nullptr, 10));
}

bool alive(pid_t pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

void lock_unlocked_x_displays() {
    for (int d = 0; d <= 32; ++d) {
        char lock[64];
        std::snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", d);
        const bool locked = ::access(lock, F_OK) == 0;
        if (locked && alive(lock_owner(lock))) continue;  // taken
        const pid_t pid = x_server_pid(d);
        // Free (or a stale lock wlroots will clear). The scan goes on past it:
        // a display taken in between (another Xwayland exiting as this one
        // starts) leaves the next free one further up.
        if (pid == 0) continue;
        if (pid < 0) continue;
        if (locked && ::unlink(lock) != 0) continue;
        const int fd = ::open(lock, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (fd < 0) continue;
        char text[12];
        std::snprintf(text, sizeof(text), "%10d\n", static_cast<int>(pid));
        const bool ok = ::write(fd, text, 11) == 11;
        ::close(fd);
        if (!ok) {
            ::unlink(lock);
            continue;
        }
        wlr_log(WLR_INFO, "brocompositor: X display :%d is served by pid %d without a lock file; locked it for them",
                d, static_cast<int>(pid));
    }
}

Rect xframe(const wlr_xwayland_surface* xs) { return Rect{xs->x, xs->y, int32_t(xs->width), int32_t(xs->height)}; }

bool xmaximized(const wlr_xwayland_surface* xs) { return xs->maximized_vert && xs->maximized_horz; }

}  // namespace

XwaylandRec* Server::xwindow(WindowId id) {
    if (id == kNoWindow) return nullptr;
    for (auto& [k, x] : xsurfaces)
        if (x->id == id) return x.get();
    return nullptr;
}

XwaylandRec* Server::xrec_of(wlr_surface* surface) {
    wlr_xwayland_surface* xs = surface ? wlr_xwayland_surface_try_from_wlr_surface(surface) : nullptr;
    if (!xs) return nullptr;
    auto it = xsurfaces.find(xs);
    return it == xsurfaces.end() ? nullptr : it->second.get();
}

namespace {

// The managed window an X window belongs to: itself or the nearest managed
// ancestor (WM_TRANSIENT_FOR chain).
WindowId managed_ancestor(Server* s, wlr_xwayland_surface* xs) {
    for (int depth = 0; xs && depth < 32; ++depth, xs = xs->parent) {
        auto it = s->xsurfaces.find(xs);
        if (it != s->xsurfaces.end() && it->second->id != kNoWindow) return it->second->id;
    }
    return kNoWindow;
}

WindowSnapshot xsnapshot(Server* s, XwaylandRec& x) {
    wlr_xwayland_surface* xs = x.xs;
    WindowSnapshot w;
    w.id = x.id;
    w.native = xs->surface ? s->surface_id(xs->surface) : 0;
    w.owner = xs->parent ? managed_ancestor(s, xs->parent) : kNoWindow;
    w.process_id = uint32_t(xs->pid);
    w.title = xs->title ? xs->title : "";
    w.app_id = xs->class_ ? xs->class_ : "";
    w.class_name = xs->instance ? xs->instance : "";
    w.frame = xframe(xs);
    int64_t best = -1;
    for (auto& [id, out] : s->outputs) {
        wlr_box ob{};
        wlr_output_layout_get_box(s->layout, out->output, &ob);
        int64_t a = Rect{ob.x, ob.y, ob.width, ob.height}.intersected(w.frame).area();
        if (a > best) {
            best = a;
            w.monitor = id;
            w.dpi = uint32_t(std::lround(96.0 * out->output->scale));
        }
    }
    w.minimized = x.minimized;
    w.maximized = xmaximized(xs);
    w.fullscreen = xs->fullscreen;
    // X11 windows get the host's frame unless they opted out of it through
    // _MOTIF_WM_HINTS (GTK client-side decorations, borderless games).
    w.decorated = xs->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    if (const xcb_size_hints_t* h = xs->size_hints) {
        bool fixed = (h->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) && (h->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) &&
                     h->min_width > 0 && h->min_width == h->max_width && h->min_height == h->max_height;
        w.resizable = !fixed;
    }
    return w;
}

}  // namespace

void Server::publish_xwindow(XwaylandRec& x, uint32_t changes) {
    if (x.id == kNoWindow) return;
    WindowSnapshot s = xsnapshot(this, x);
    if (s.monitor != x.snap.monitor) changes |= change::Monitor;
    if (!(s.frame == x.snap.frame)) changes |= change::Geometry;
    bool differs = !(s == x.snap);
    x.snap = s;
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        auto it = mirror.windows.find(x.id);
        if (it != mirror.windows.end()) it->second.snap = s;
    }
    foreign_update(x.foreign.get(), s, x.activated, x.minimized, x.entered);
    if (differs || changes) events.push(WindowChanged{s, changes});
}

void Server::update_xwindow_outputs(XwaylandRec& x) {
    if (x.id == kNoWindow || !x.xs->surface) return;
    Rect frame = xframe(x.xs);
    std::set<wlr_output*> now;
    for (auto& [id, out] : outputs) {
        if (!out->output->enabled) continue;
        wlr_box ob{};
        wlr_output_layout_get_box(layout, out->output, &ob);
        if (Rect{ob.x, ob.y, ob.width, ob.height}.intersects(frame)) now.insert(out->output);
    }
    for (wlr_output* o : x.entered)
        if (!now.count(o)) wlr_surface_send_leave(x.xs->surface, o);
    for (wlr_output* o : now)
        if (!x.entered.count(o)) wlr_surface_send_enter(x.xs->surface, o);
    x.entered = std::move(now);
    foreign_update(x.foreign.get(), x.snap, x.activated, x.minimized, x.entered);
}

std::vector<SurfaceNode> Server::xwindow_tree(XwaylandRec& x) {
    if (!x.xs->surface) return {};
    return build_tree(x.xs->surface, Point{0, 0}, nullptr, nullptr);
}

bool Server::place_xwindow(XwaylandRec& x, const Rect& frame) {
    int32_t w = frame.width > 0 ? frame.width : x.xs->width;
    int32_t h = frame.height > 0 ? frame.height : x.xs->height;
    wlr_xwayland_surface_configure(x.xs, int16_t(frame.x), int16_t(frame.y), uint16_t(std::max(1, w)),
                                   uint16_t(std::max(1, h)));
    x.positioned = true;
    publish_xwindow(x, change::Geometry);
    update_xwindow_outputs(x);
    if (x.xs->surface) mark_tree_dirty(x.xs->surface);
    return true;
}

void Server::set_xwindow_visible(XwaylandRec& x, bool visible) { x.visible = visible; }

void Server::close_xwindow(XwaylandRec& x) { wlr_xwayland_surface_close(x.xs); }

void Server::set_xwindow_state(XwaylandRec& x, bool maximized, bool fullscreen) {
    wlr_xwayland_surface_set_maximized(x.xs, maximized);
    wlr_xwayland_surface_set_fullscreen(x.xs, fullscreen);
    publish_xwindow(x, change::State);
}

void Server::set_xwindow_minimized(XwaylandRec& x, bool minimized) {
    x.minimized = minimized;
    wlr_xwayland_surface_set_minimized(x.xs, minimized);
    publish_xwindow(x, change::State);
}

void Server::activate_xwindow(XwaylandRec& x, bool activated) {
    x.activated = activated;
    if (activated) {
        if (x.minimized) set_xwindow_minimized(x, false);
        wlr_xwayland_surface_restack(x.xs, nullptr, XCB_STACK_MODE_ABOVE);
    }
    // Sets (or drops) the X input focus and _NET_ACTIVE_WINDOW.
    wlr_xwayland_surface_activate(x.xs, activated);
    foreign_update(x.foreign.get(), x.snap, x.activated, x.minimized, x.entered);
}

void Server::publish_unmanaged() {
    std::vector<UnmanagedSurfaceInfo> list;
    for (XwaylandRec* x : unmanaged_order) list.push_back(x->uinfo);
    std::lock_guard<std::mutex> lock(mirror.m);
    mirror.unmanaged = std::move(list);
}

void Server::update_xwayland_workareas() {
    // wlroots dereferences the window manager: only once X is up (ready).
    if (!xwayland || !xwayland->xwm) return;
    std::vector<wlr_box> boxes;
    for (auto& [id, out] : outputs) {
        if (!out->output->enabled) continue;
        wlr_box b{};
        wlr_output_layout_get_box(layout, out->output, &b);
        if (!out->work_area.empty()) b = wlr_box{out->work_area.x, out->work_area.y, out->work_area.width,
                                                 out->work_area.height};
        boxes.push_back(b);
    }
    if (!boxes.empty()) wlr_xwayland_set_workareas(xwayland, boxes.data(), boxes.size());
}

namespace {

// Windows map in any order: a transient mapped before its owner names it
// once the owner has an id (and loses it when the owner unmaps).
void republish_transients(Server* s) {
    for (auto& [xs, rec] : s->xsurfaces)
        if (rec->id != kNoWindow && xs->parent) s->publish_xwindow(*rec, 0);
}

void map_managed(Server* s, XwaylandRec* x) {
    wlr_xwayland_surface* xs = x->xs;
    x->id = s->next_window++;
    if (!x->positioned) {
        Size g{xs->width, xs->height};
        Size min;
        bool client_pos = false;
        if (const xcb_size_hints_t* h = xs->size_hints) {
            if (h->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) min = Size{h->min_width, h->min_height};
            // A position the user / program asked for (geometry option,
            // restored session) is honoured when it is on screen.
            client_pos = (h->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION)) &&
                         (xs->x != 0 || xs->y != 0) &&
                         wlr_output_layout_output_at(s->layout, xs->x + 1, xs->y + 1) != nullptr;
        }
        Rect f = client_pos ? xframe(xs) : s->initial_frame(g, managed_ancestor(s, xs->parent), min);
        wlr_xwayland_surface_configure(xs, int16_t(f.x), int16_t(f.y), uint16_t(std::max(1, f.width)),
                                       uint16_t(std::max(1, f.height)));
        x->positioned = true;
    }
    x->visible = true;
    x->snap = xsnapshot(s, *x);
    {
        std::lock_guard<std::mutex> lock(s->mirror.m);
        WindowMirror m;
        m.snap = x->snap;
        m.visible = true;
        m.root = x->snap.native;
        m.ssd = x->snap.decorated;
        s->mirror.windows[x->id] = m;
    }
    s->events.push(WindowAdded{x->snap});
    s->foreign_map(x->foreign, x->id);
    s->update_xwindow_outputs(*x);
    s->mark_tree_dirty(xs->surface);
    s->schedule_idle_check();
    republish_transients(s);
}

void unmap_managed(Server* s, XwaylandRec* x) {
    WindowId id = x->id;
    x->id = kNoWindow;
    x->entered.clear();
    x->positioned = false;
    s->foreign_unmap(x->foreign);
    {
        std::lock_guard<std::mutex> lock(s->mirror.m);
        s->mirror.windows.erase(id);
    }
    if (s->focused_window == id) {
        s->focused_window = kNoWindow;
        if (!s->locked()) wlr_seat_keyboard_notify_clear_focus(s->seat);
    }
    s->events.push(WindowRemoved{id});
    s->schedule_idle_check();
    s->notify_window_commit(id);  // window captures waiting on it fail (stopped)
    republish_transients(s);
}

void map_unmanaged(Server* s, XwaylandRec* x) {
    wlr_xwayland_surface* xs = x->xs;
    x->unmanaged = s->next_unmanaged++;
    UnmanagedSurfaceInfo& u = x->uinfo;
    u.id = x->unmanaged;
    u.surface = s->surface_id(xs->surface);
    u.rect = xframe(xs);
    u.process_id = uint32_t(xs->pid);
    u.owner = xs->parent ? managed_ancestor(s, xs->parent) : kNoWindow;
    if (u.owner == kNoWindow) {
        // Menus rarely set WM_TRANSIENT_FOR: attribute them to the focused
        // window of the same client.
        if (XwaylandRec* f = s->xwindow(s->focused_window))
            if (f->xs->pid == xs->pid) u.owner = f->id;
    }
    s->unmanaged_order.push_back(x);
    s->publish_unmanaged();
    s->server_events.push(UnmanagedSurfaceAdded{u});
    if (wlr_xwayland_or_surface_wants_focus(xs) && !s->locked()) s->focus_surface(xs->surface);
}

void unmap_unmanaged(Server* s, XwaylandRec* x) {
    UnmanagedId id = x->unmanaged;
    x->unmanaged = 0;
    std::erase(s->unmanaged_order, x);
    s->publish_unmanaged();
    s->server_events.push(UnmanagedSurfaceRemoved{id});
    // A focused menu hands the keyboard back to the focused window.
    if (s->seat->keyboard_state.focused_surface == x->xs->surface && !s->locked()) {
        WindowId w = s->focused_window;
        s->focused_window = kNoWindow;
        s->focus_window(w);
    }
}

void update_unmanaged_rect(Server* s, XwaylandRec* x) {
    if (!x->unmanaged) return;
    Rect r = xframe(x->xs);
    if (r == x->uinfo.rect) return;
    x->uinfo.rect = r;
    s->publish_unmanaged();
    s->server_events.push(UnmanagedSurfaceChanged{x->uinfo});
}

void on_new_xsurface(Server* s, wlr_xwayland_surface* xs) {
    auto rec = std::make_unique<XwaylandRec>();
    XwaylandRec* x = rec.get();
    x->srv = s;
    x->xs = xs;
    s->xsurfaces[xs] = std::move(rec);

    x->associate.connect(&xs->events.associate, [s, x](void*) {
        wlr_surface* surface = x->xs->surface;
        x->map.connect(&surface->events.map, [s, x](void*) {
            if (x->xs->override_redirect) map_unmanaged(s, x);
            else map_managed(s, x);
        });
        x->unmap.connect(&surface->events.unmap, [s, x](void*) {
            if (x->unmanaged) unmap_unmanaged(s, x);
            if (x->id != kNoWindow) unmap_managed(s, x);
        });
        x->commit.connect(&surface->events.commit, [s, x](void*) {
            if (x->unmanaged) update_unmanaged_rect(s, x);
            if (x->id != kNoWindow && !(xframe(x->xs) == x->snap.frame)) {
                s->publish_xwindow(*x, change::Geometry);
                s->update_xwindow_outputs(*x);
            }
        });
        // wlroots maps an X surface on its first commit after association.
        // When Xwayland committed the window's buffer before the xwm paired
        // the surface with the X window (the two arrive on different
        // connections, so a loaded host sees this), no later commit comes
        // for a window that does not redraw, and it would never map. Map
        // it now, as that commit would have.
        if (wlr_surface_has_buffer(surface) && !surface->mapped) wlr_surface_map(surface);
    });
    x->dissociate.connect(&xs->events.dissociate, [s, x](void*) {
        if (x->unmanaged) unmap_unmanaged(s, x);
        if (x->id != kNoWindow) unmap_managed(s, x);
        x->map.disconnect();
        x->unmap.disconnect();
        x->commit.disconnect();
    });
    x->destroy.connect(&xs->events.destroy, [s, xs](void*) {
        auto it = s->xsurfaces.find(xs);
        if (it == s->xsurfaces.end()) return;
        XwaylandRec* r = it->second.get();
        if (r->unmanaged) unmap_unmanaged(s, r);
        if (r->id != kNoWindow) unmap_managed(s, r);
        s->foreign_unmap(r->foreign);
        s->xsurfaces.erase(it);
        s->mark_all_trees_dirty();
    });

    x->req_configure.connect(&xs->events.request_configure, [s, x](void* data) {
        auto* e = static_cast<wlr_xwayland_surface_configure_event*>(data);
        if (x->xs->override_redirect || x->id == kNoWindow) {
            // Unmapped windows pick their initial size; unmanaged ones place
            // themselves.
            wlr_xwayland_surface_configure(x->xs, e->x, e->y, e->width, e->height);
            update_unmanaged_rect(s, x);
            return;
        }
        // A mapped managed window may resize itself; its position is the
        // host's.
        wlr_xwayland_surface_configure(x->xs, x->xs->x, x->xs->y, e->width, e->height);
        s->publish_xwindow(*x, change::Geometry);
        s->update_xwindow_outputs(*x);
        s->mark_tree_dirty(x->xs->surface);
    });
    x->set_geometry.connect(&xs->events.set_geometry, [s, x](void*) { update_unmanaged_rect(s, x); });
    x->req_move.connect(&xs->events.request_move, [s, x](void*) {
        s->push_window_request(x->id, WindowRequestKind::Move, false);
    });
    x->req_resize.connect(&xs->events.request_resize, [s, x](void* data) {
        auto* e = static_cast<wlr_xwayland_resize_event*>(data);
        if (x->id == kNoWindow) return;
        WindowRequest r;
        r.window = x->id;
        r.kind = WindowRequestKind::Resize;
        r.edges = e->edges;
        s->server_events.push(r);
    });
    x->req_minimize.connect(&xs->events.request_minimize, [s, x](void* data) {
        auto* e = static_cast<wlr_xwayland_minimize_event*>(data);
        s->push_window_request(x->id, e->minimize ? WindowRequestKind::Minimize : WindowRequestKind::Unminimize,
                               false);
    });
    // _NET_WM_STATE requests: the xwm already recorded the wanted state; the
    // host grants it with set_window_state().
    x->req_maximize.connect(&xs->events.request_maximize, [s, x](void*) {
        s->push_window_request(x->id,
                               xmaximized(x->xs) ? WindowRequestKind::Maximize : WindowRequestKind::Unmaximize, false);
    });
    x->req_fullscreen.connect(&xs->events.request_fullscreen, [s, x](void*) {
        s->push_window_request(x->id,
                               x->xs->fullscreen ? WindowRequestKind::Fullscreen : WindowRequestKind::Unfullscreen,
                               false);
    });
    x->req_activate.connect(&xs->events.request_activate, [s, x](void*) {
        s->push_window_request(x->id, WindowRequestKind::Activate, false);
    });
    x->set_title.connect(&xs->events.set_title, [s, x](void*) { s->publish_xwindow(*x, change::Title); });
    x->set_class.connect(&xs->events.set_class, [s, x](void*) { s->publish_xwindow(*x, change::Title); });
    x->set_parent.connect(&xs->events.set_parent, [s, x](void*) { s->publish_xwindow(*x, 0); });
    x->set_decorations.connect(&xs->events.set_decorations, [s, x](void*) {
        if (x->id == kNoWindow) return;
        {
            std::lock_guard<std::mutex> lock(s->mirror.m);
            auto it = s->mirror.windows.find(x->id);
            if (it != s->mirror.windows.end())
                it->second.ssd = x->xs->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
        }
        s->publish_xwindow(*x, change::State);
    });
}

}  // namespace

bool Server::init_xwayland(std::string* error) {
    (void)error;
    if (config.xwayland == XwaylandMode::Off) return true;
    lock_unlocked_x_displays();
    xwayland = wlr_xwayland_create(display, compositor, config.xwayland == XwaylandMode::Lazy);
    if (!xwayland) {
        // Not fatal: the Wayland session works without X11 clients.
        wlr_log(WLR_ERROR, "brocompositor: XWayland unavailable");
        return true;
    }
    wlr_xwayland_set_seat(xwayland, seat);
    std::string name = xwayland->display_name ? xwayland->display_name : "";
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.xwayland_display = name;
    }
    server_events.push(XwaylandStatus{name, false});
    xwl_ready.connect(&xwayland->events.ready, [this](void*) {
        update_xwayland_workareas();
        std::string n = xwayland->display_name ? xwayland->display_name : "";
        server_events.push(XwaylandStatus{n, true});
    });
    xwl_new_surface.connect(&xwayland->events.new_surface, [this](void* data) {
        on_new_xsurface(this, static_cast<wlr_xwayland_surface*>(data));
    });
    return true;
}

void Server::shutdown_xwayland() {
    xwl_ready.disconnect();
    xwl_new_surface.disconnect();
    unmanaged_order.clear();
    xsurfaces.clear();
    if (xwayland) wlr_xwayland_destroy(xwayland);
    xwayland = nullptr;
}

#else  // !BC_HAVE_XWAYLAND: wlroots was built without XWayland.

XwaylandRec* Server::xwindow(WindowId) { return nullptr; }
XwaylandRec* Server::xrec_of(wlr_surface*) { return nullptr; }
void Server::publish_xwindow(XwaylandRec&, uint32_t) {}
void Server::update_xwindow_outputs(XwaylandRec&) {}
std::vector<SurfaceNode> Server::xwindow_tree(XwaylandRec&) { return {}; }
bool Server::place_xwindow(XwaylandRec&, const Rect&) { return false; }
void Server::set_xwindow_visible(XwaylandRec&, bool) {}
void Server::close_xwindow(XwaylandRec&) {}
void Server::set_xwindow_state(XwaylandRec&, bool, bool) {}
void Server::set_xwindow_minimized(XwaylandRec&, bool) {}
void Server::activate_xwindow(XwaylandRec&, bool) {}
void Server::publish_unmanaged() {}
void Server::update_xwayland_workareas() {}
bool Server::init_xwayland(std::string*) {
    if (config.xwayland != XwaylandMode::Off)
        wlr_log(WLR_INFO, "brocompositor: wlroots was built without XWayland; X11 clients are not supported");
    return true;
}
void Server::shutdown_xwayland() {}

#endif

}  // namespace brocompositor::wl
