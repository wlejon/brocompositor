// Pointer and keyboard extras: pointer constraints + relative pointer
// (games, 3D viewports), virtual keyboard / pointer devices from clients
// (wtype, wlrctl, IMEs, remote desktop) and keyboard-shortcuts-inhibit.
#include "linux/server_impl.h"

#include <cmath>

namespace brocompositor::wl {

void Server::init_pointer_extras() {
    pointer_constraints = wlr_pointer_constraints_v1_create(display);
    relative_pointer = wlr_relative_pointer_manager_v1_create(display);
    new_constraint.connect(&pointer_constraints->events.new_constraint, [this](void* data) {
        auto* c = static_cast<wlr_pointer_constraint_v1*>(data);
        auto rec = std::make_unique<ConstraintRec>();
        ConstraintRec* r = rec.get();
        r->constraint = c;
        r->destroy.connect(&c->events.destroy, [this, c](void*) {
            if (active_constraint == c) {
                active_constraint = nullptr;
                server_events.push(PointerConstraintChanged{surface_id(c->surface), PointerConstraintKind::None});
            }
            constraints.erase(c);
        });
        r->set_region.connect(&c->events.set_region, [](void*) {});
        constraints[c] = std::move(rec);
        update_constraint();
    });

    virtual_keyboard = wlr_virtual_keyboard_manager_v1_create(display);
    new_virtual_keyboard.connect(&virtual_keyboard->events.new_virtual_keyboard, [this](void* data) {
        auto* vk = static_cast<wlr_virtual_keyboard_v1*>(data);
        add_keyboard(&vk->keyboard, true);
    });
    virtual_pointer = wlr_virtual_pointer_manager_v1_create(display);
    new_virtual_pointer.connect(&virtual_pointer->events.new_virtual_pointer, [this](void* data) {
        auto* e = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
        add_pointer(&e->new_pointer->pointer);
        if (!pointers.empty()) {
            PointerRec* r = pointers.back().get();
            r->mapped_output = e->suggested_output;
            r->origin = InputOrigin::Client;
            r->owner = wl_resource_get_client(e->new_pointer->resource);
            r->owner_pid = client_pid(r->owner);
        }
    });

    shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(display);
    new_shortcuts_inhibitor.connect(&shortcuts_inhibit->events.new_inhibitor, [this](void* data) {
        auto* in = static_cast<wlr_keyboard_shortcuts_inhibitor_v1*>(data);
        auto rec = std::make_unique<ShortcutsInhibitorRec>();
        rec->inhibitor = in;
        rec->destroy.connect(&in->events.destroy, [this, in](void*) {
            if (active_inhibitor == in) {
                active_inhibitor = nullptr;
                server_events.push(ShortcutsInhibitChanged{surface_id(in->surface), false});
            }
            inhibitors.erase(in);
            update_shortcuts_inhibit();
        });
        inhibitors[in] = std::move(rec);
        update_shortcuts_inhibit();
    });
}

bool Server::constrain_motion(double dx, double dy, double udx, double udy, uint32_t time, double* nx, double* ny) {
    *nx = cursor_x + dx;
    *ny = cursor_y + dy;
    // Relative motion reaches the pointer-focused client whatever the
    // constraint (that is the point of locking).
    if (seat->pointer_state.focused_surface && (dx != 0 || dy != 0 || udx != 0 || udy != 0))
        wlr_relative_pointer_manager_v1_send_relative_motion(relative_pointer, seat, uint64_t(time) * 1000, dx, dy,
                                                             udx, udy);
    wlr_pointer_constraint_v1* c = active_constraint;
    if (!c) return true;
    if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) return false;
    // Confined: keep the surface-local position inside the region.
    double sx = cursor_x - pointer_origin_x, sy = cursor_y - pointer_origin_y;
    double tx = sx + dx, ty = sy + dy;
    double ox = tx, oy = ty;
    if (!wlr_region_confine(&c->region, sx, sy, tx, ty, &ox, &oy)) {
        ox = sx;
        oy = sy;
    }
    *nx = pointer_origin_x + ox;
    *ny = pointer_origin_y + oy;
    return true;
}

void Server::update_constraint() {
    wlr_surface* focus = seat->pointer_state.focused_surface;
    wlr_pointer_constraint_v1* c =
        focus ? wlr_pointer_constraints_v1_constraint_for_surface(pointer_constraints, focus, seat) : nullptr;
    if (c == active_constraint) return;
    if (wlr_pointer_constraint_v1* old = active_constraint) {
        // A lock may leave the cursor where the client drew it.
        if (old->type == WLR_POINTER_CONSTRAINT_V1_LOCKED && old->current.cursor_hint.enabled &&
            old->surface == focus)
            warp(pointer_origin_x + old->current.cursor_hint.x, pointer_origin_y + old->current.cursor_hint.y);
        SurfaceId old_surface = surface_id(old->surface);
        active_constraint = nullptr;
        // Destroys a oneshot constraint (and its record): `old` is gone after.
        wlr_pointer_constraint_v1_send_deactivated(old);
        if (!c) server_events.push(PointerConstraintChanged{old_surface, PointerConstraintKind::None});
    }
    if (!c) return;
    active_constraint = c;
    wlr_pointer_constraint_v1_send_activated(c);
    server_events.push(PointerConstraintChanged{
        surface_id(c->surface), c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? PointerConstraintKind::Locked
                                                                            : PointerConstraintKind::Confined});
}

void Server::update_shortcuts_inhibit() {
    wlr_surface* focus = seat->keyboard_state.focused_surface;
    wlr_keyboard_shortcuts_inhibitor_v1* want = nullptr;
    for (auto& [in, rec] : inhibitors)
        if (in->surface == focus && in->seat == seat) want = in;
    if (want == active_inhibitor) return;
    if (active_inhibitor) {
        wlr_keyboard_shortcuts_inhibitor_v1_deactivate(active_inhibitor);
        server_events.push(ShortcutsInhibitChanged{surface_id(active_inhibitor->surface), false});
    }
    active_inhibitor = want;
    if (want) {
        wlr_keyboard_shortcuts_inhibitor_v1_activate(want);
        server_events.push(ShortcutsInhibitChanged{surface_id(want->surface), true});
    }
}

}  // namespace brocompositor::wl
