// Clipboard (wl_data_device), primary selection, wlr-data-control (clipboard
// managers, wl-clipboard) and drag-and-drop. wlroots moves the data between
// clients; the server accepts requests and reports what is on offer.
#include "linux/server_impl.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <type_traits>

namespace brocompositor::wl {

namespace {

std::vector<std::string> mime_list(const wl_array* a) {
    std::vector<std::string> out;
    const char* const* p;
    for (p = static_cast<const char* const*>(a->data);
         reinterpret_cast<const char*>(p) < static_cast<const char*>(a->data) + a->size; ++p)
        out.emplace_back(*p);
    return out;
}

}  // namespace

void Server::init_selection() {
    wlr_data_device_manager_create(display);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_data_control_manager_v1_create(display);

    seat_request_selection.connect(&seat->events.request_set_selection, [this](void* data) {
        auto* e = static_cast<wlr_seat_request_set_selection_event*>(data);
        wlr_seat_set_selection(seat, e->source, e->serial);
    });
    seat_request_primary.connect(&seat->events.request_set_primary_selection, [this](void* data) {
        auto* e = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
        wlr_seat_set_primary_selection(seat, e->source, e->serial);
    });
    selection_changed.connect(&seat->events.set_selection, [this](void*) {
        SelectionChanged ev;
        ev.primary = false;
        if (seat->selection_source) ev.mime_types = mime_list(&seat->selection_source->mime_types);
        server_events.push(ev);
    });
    primary_changed.connect(&seat->events.set_primary_selection, [this](void*) {
        SelectionChanged ev;
        ev.primary = true;
        if (seat->primary_selection_source) ev.mime_types = mime_list(&seat->primary_selection_source->mime_types);
        server_events.push(ev);
    });

    seat_request_drag.connect(&seat->events.request_start_drag, [this](void* data) {
        auto* e = static_cast<wlr_seat_request_start_drag_event*>(data);
        if (wlr_seat_validate_pointer_grab_serial(seat, e->origin, e->serial)) {
            wlr_seat_start_pointer_drag(seat, e->drag, e->serial);
        } else if (e->drag->source) {
            wlr_data_source_destroy(e->drag->source);
        }
    });
    seat_start_drag.connect(&seat->events.start_drag, [this](void* data) {
        auto* drag = static_cast<wlr_drag*>(data);
        drag_icon = drag->icon ? drag->icon->surface : nullptr;
        drag_icon_offset = Point{};
        if (drag_icon) {
            // Where the icon sits against the pointer moves with each commit's
            // offset (wl_surface.offset, or attach's dx/dy).
            drag_icon_commit.connect(&drag_icon->events.commit, [this](void*) { publish_drag_icon(true); });
        }
        publish_drag_icon(drag_icon != nullptr);
        server_events.push(DragIconChanged{drag_icon ? surface_id(drag_icon) : kNoSurface});
        drag_icon_destroy.connect(&drag->events.destroy, [this](void*) {
            clear_drag_icon();
            drag_icon_destroy.disconnect();
            server_events.push(DragIconChanged{kNoSurface});
        });
    });
}

void Server::publish_drag_icon(bool add_offset) {
    if (!drag_icon) return;
    if (add_offset) {
        drag_icon_offset.x += drag_icon->current.dx;
        drag_icon_offset.y += drag_icon->current.dy;
    }
    SurfaceNode node;
    node.surface = surface_id(drag_icon);
    node.offset = drag_icon_offset;
    node.size = Size{drag_icon->current.width, drag_icon->current.height};
    std::lock_guard<std::mutex> lock(mirror.m);
    mirror.drag_icon = node;
}

void Server::clear_drag_icon() {
    drag_icon_commit.disconnect();
    drag_icon = nullptr;
    std::lock_guard<std::mutex> lock(mirror.m);
    mirror.drag_icon.reset();
}

// ---------------------------------------------------------------- the host's drag
//
// The host's UI as a wl_data_source: wlroots offers it to the client under
// the pointer like any client's source, and asks it for the data, which it
// writes from its own copy. Dropped on a client that took it, the source
// lives until that client finishes with the offer (or lets it go); wlroots
// then destroys it, and its end is reported with the action the client
// chose. Released anywhere else, wlroots destroys it at once.

struct Server::HostDataSource {
    wlr_data_source base;  // first: wlroots holds &base, which is the source's address
    Server* srv = nullptr;  // null: never started, nothing to report
    uint64_t id = 0;
    std::vector<std::pair<std::string, std::string>>* data = nullptr;
    bool dropped = false;
    bool finished = false;
    uint32_t action = 0;
};
static_assert(std::is_standard_layout_v<Server::HostDataSource>);

namespace {

Server::HostDataSource* host_source(wlr_data_source* s) { return reinterpret_cast<Server::HostDataSource*>(s); }

uint32_t now_msec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
}

// The bytes go out from a thread of their own: a target that reads slowly
// (or never) must not stall the server. SIGPIPE is blocked there, so a
// target that closes early costs that thread an EPIPE, not the process.
void write_and_close(int fd, std::string bytes) {
    std::thread([fd, bytes = std::move(bytes)] {
        sigset_t pipe_only;
        sigemptyset(&pipe_only);
        sigaddset(&pipe_only, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_only, nullptr);
        const int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += size_t(n);
        }
        ::close(fd);
    }).detach();
}

const wlr_data_source_impl kHostSourceImpl = {
    .send =
        [](wlr_data_source* s, const char* mime_type, int32_t fd) {
            auto* h = host_source(s);
            for (const auto& [mime, bytes] : *h->data)
                if (mime == mime_type) {
                    write_and_close(fd, bytes);
                    return;
                }
            ::close(fd);
        },
    .accept = nullptr,
    .destroy = [](wlr_data_source* s) { host_source(s)->srv->host_drag_ended(host_source(s)); },
    .dnd_drop = [](wlr_data_source* s) { host_source(s)->dropped = true; },
    .dnd_finish =
        [](wlr_data_source* s) {
            auto* h = host_source(s);
            h->finished = true;
            h->action = s->current_dnd_action;
        },
    .dnd_action = nullptr,
};

}  // namespace

uint64_t Server::start_host_drag(ServerBackend::HostDrag d) {
    if (seat->drag || d.data.empty()) return 0;
    // An earlier drag dropped on a client that has neither finished nor let
    // the offer go: it ends now (reported, under its own id).
    if (host_drag) wlr_data_source_destroy(&host_drag->base);
    auto* h = new HostDataSource;
    h->data = new std::vector<std::pair<std::string, std::string>>(std::move(d.data));
    wlr_data_source_init(&h->base, &kHostSourceImpl);
    for (const auto& [mime, bytes] : *h->data) {
        auto* slot = static_cast<char**>(wl_array_add(&h->base.mime_types, sizeof(char*)));
        if (slot) *slot = strdup(mime.c_str());
    }
    h->base.actions = int32_t(d.actions & 7u);
    host_seat_client.seat = seat;
    wlr_drag* drag = wlr_drag_create(&host_seat_client, &h->base, nullptr);
    if (!drag) {
        wlr_data_source_destroy(&h->base);  // never started: nothing reported
        return 0;
    }
    h->srv = this;
    h->id = ++host_drag_serial;
    host_drag = h;
    // The press that began the drag went to the host's UI, never to the
    // seat: the seat holds the button from here, so the drag's grab ends it
    // on that button's release. Nothing has the pointer meanwhile.
    wlr_seat_pointer_clear_focus(seat);
    wlr_seat_pointer_notify_button(seat, now_msec(), d.button, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_start_pointer_drag(seat, drag, wl_display_next_serial(display));
    return h->id;
}

void Server::cancel_host_drag() {
    if (!host_drag || !seat->drag || seat->drag->source != &host_drag->base) return;
    // No target, then the held button up: the drag's grab ends it there.
    wlr_seat_pointer_notify_clear_focus(seat);
    const uint32_t button = seat->pointer_state.grab_button;
    wlr_seat_pointer_notify_button(seat, now_msec(), button, WL_POINTER_BUTTON_STATE_RELEASED);
}

void Server::host_drag_ended(HostDataSource* h) {
    if (host_drag == h) host_drag = nullptr;
    if (h->srv) server_events.push(HostDragEnded{h->id, h->dropped, h->finished ? h->action : 0u});
    delete h->data;
    delete h;
}

}  // namespace brocompositor::wl
