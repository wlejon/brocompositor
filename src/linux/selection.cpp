// Clipboard (wl_data_device), primary selection, wlr-data-control (clipboard
// managers, wl-clipboard) and drag-and-drop. wlroots moves the data between
// clients; the server accepts requests and reports what is on offer.
#include "linux/server_impl.h"

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

}  // namespace brocompositor::wl
