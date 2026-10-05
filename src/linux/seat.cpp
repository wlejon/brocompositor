// Seat: input devices (libinput / nested / virtual), the cursor, keyboard
// focus and the host-routed delivery of pointer and keyboard input.
#include "linux/server_impl.h"

#include <ctime>

namespace brocompositor::wl {

namespace {

uint32_t now_msec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
}

const wlr_keyboard_impl kVirtualKeyboardImpl = {"brocompositor-virtual-keyboard", nullptr};
const wlr_pointer_impl kVirtualPointerImpl = {"brocompositor-virtual-pointer"};

}  // namespace

// The cursor position is ours (wlr_cursor would drive wlr_output_cursor,
// which needs a wlr_renderer): relative motion accumulates and is clamped to
// the output layout; absolute devices map onto the layout's bounding box.
void Server::warp(double x, double y) {
    if (!wlr_output_layout_output_at(layout, x, y)) {
        double cx = x, cy = y;
        wlr_output_layout_closest_point(layout, nullptr, x, y, &cx, &cy);
        x = cx;
        y = cy;
    }
    cursor_x = x;
    cursor_y = y;
    publish_cursor_position();
}

void Server::send_modifiers(const KeyboardModifiers& m) {
    if (m == sent_modifiers) return;
    sent_modifiers = m;
    wlr_keyboard_modifiers wm{};
    wm.depressed = m.depressed;
    wm.latched = m.latched;
    wm.locked = m.locked;
    wm.group = m.group;
    wlr_seat_keyboard_notify_modifiers(seat, &wm);
}

void Server::init_seat() {
    seat = wlr_seat_create(display, "seat0");

    seat_request_cursor.connect(&seat->events.request_set_cursor, [this](void* data) {
        auto* e = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
        if (e->seat_client != seat->pointer_state.focused_client) return;
        CursorChanged c;
        c.surface = e->surface ? surface_id(e->surface) : kNoSurface;
        c.hotspot = Point{e->hotspot_x, e->hotspot_y};
        c.hidden = e->surface == nullptr;
        set_cursor(c);
    });

    cursor_shape = wlr_cursor_shape_manager_v1_create(display, 1);
    request_cursor_shape.connect(&cursor_shape->events.request_set_shape, [this](void* data) {
        auto* e = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
        if (e->seat_client != seat->pointer_state.focused_client) return;
        CursorChanged c;
        c.shape = wlr_cursor_shape_v1_name(e->shape);
        set_cursor(c);
    });

    new_input.connect(&backend->events.new_input,
                      [this](void* data) { on_new_input(static_cast<wlr_input_device*>(data)); });

    // Virtual devices: the seat always has a keyboard and a pointer.
    vkeyboard = new wlr_keyboard{};
    wlr_keyboard_init(vkeyboard, &kVirtualKeyboardImpl, "brocompositor-virtual-keyboard");
    add_keyboard(vkeyboard, true);
    vpointer = new wlr_pointer{};
    wlr_pointer_init(vpointer, &kVirtualPointerImpl, "brocompositor-virtual-pointer");
    add_pointer(vpointer);
}

void Server::on_new_input(wlr_input_device* device) {
    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD: add_keyboard(wlr_keyboard_from_input_device(device), false); break;
        case WLR_INPUT_DEVICE_POINTER: add_pointer(wlr_pointer_from_input_device(device)); break;
        case WLR_INPUT_DEVICE_TOUCH: add_touch(wlr_touch_from_input_device(device), false); break;
        case WLR_INPUT_DEVICE_TABLET: add_tablet(wlr_tablet_from_input_device(device)); break;
        case WLR_INPUT_DEVICE_TABLET_PAD: add_tablet_pad(wlr_tablet_pad_from_input_device(device)); break;
        default: break;
    }
}

void Server::add_keyboard(wlr_keyboard* kb, bool is_virtual) {
    // virtual-keyboard-v1 devices (wtype, IMEs) bring their own keymap.
    wlr_virtual_keyboard_v1* vk = wlr_input_device_get_virtual_keyboard(&kb->base);
    if (!vk) {
        xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_rule_names names{};
        auto opt = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
        names.rules = opt(config.xkb_rules);
        names.model = opt(config.xkb_model);
        names.layout = opt(config.xkb_layout);
        names.variant = opt(config.xkb_variant);
        names.options = opt(config.xkb_options);
        xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap) {
            wlr_keyboard_set_keymap(kb, keymap);
            xkb_keymap_unref(keymap);
        }
        xkb_context_unref(ctx);
        wlr_keyboard_set_repeat_info(kb, config.repeat_rate, config.repeat_delay);
    }

    auto r = std::make_unique<KeyboardRec>();
    KeyboardRec* k = r.get();
    k->srv = this;
    k->keyboard = kb;
    k->is_virtual = is_virtual || vk;
    if (vk) k->owner = wl_resource_get_client(vk->resource);
    if (kb->keymap) k->shadow = xkb_state_new(kb->keymap);
    k->keymap.connect(&kb->events.keymap, [k](void*) {
        if (k->shadow) xkb_state_unref(k->shadow);
        k->shadow = k->keyboard->keymap ? xkb_state_new(k->keyboard->keymap) : nullptr;
    });
    // The key signal fires before wlroots updates the device's xkb state, so
    // the shadow state (same keymap, same key sequence) yields the modifiers
    // that follow this key. Modifiers reach clients only with routed keys
    // (keyboard_key) or keyboard_modifiers(), keeping them in order.
    k->key.connect(&kb->events.key, [this, kb, k](void* data) {
        auto* e = static_cast<wlr_keyboard_key_event*>(data);
        wlr_seat_set_keyboard(seat, kb);
        activity();
        KeyboardKey ev;
        ev.shortcuts_inhibited = shortcuts_inhibited();
        ev.time_msec = e->time_msec;
        ev.keycode = e->keycode;
        ev.pressed = e->state == WL_KEYBOARD_KEY_STATE_PRESSED;
        ev.keysym = kb->xkb_state ? xkb_state_key_get_one_sym(kb->xkb_state, e->keycode + 8) : 0;
        ev.modifiers = wlr_keyboard_get_modifiers(kb);
        if (k->shadow) {
            if (e->update_state)
                xkb_state_update_key(k->shadow, e->keycode + 8, ev.pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
            ev.modifiers_after.depressed = xkb_state_serialize_mods(k->shadow, XKB_STATE_MODS_DEPRESSED);
            ev.modifiers_after.latched = xkb_state_serialize_mods(k->shadow, XKB_STATE_MODS_LATCHED);
            ev.modifiers_after.locked = xkb_state_serialize_mods(k->shadow, XKB_STATE_MODS_LOCKED);
            ev.modifiers_after.group = xkb_state_serialize_layout(k->shadow, XKB_STATE_LAYOUT_EFFECTIVE);
        }
        server_events.push(ev);
    });
    k->destroy.connect(&kb->base.events.destroy, [this, k](void*) {
        if (wlr_seat_get_keyboard(seat) == k->keyboard) wlr_seat_set_keyboard(seat, vkeyboard != k->keyboard ? vkeyboard : nullptr);
        keyboards.remove_if([k](const std::unique_ptr<KeyboardRec>& p) { return p.get() == k; });
    });
    keyboards.push_back(std::move(r));
    if (!wlr_seat_get_keyboard(seat) || !is_virtual) wlr_seat_set_keyboard(seat, kb);
    update_capabilities();
}

void Server::add_pointer(wlr_pointer* p) {
    auto r = std::make_unique<PointerRec>();
    PointerRec* raw = r.get();
    raw->srv = this;
    raw->pointer = p;
    raw->motion.connect(&p->events.motion, [this](void* data) {
        auto* e = static_cast<wlr_pointer_motion_event*>(data);
        activity();
        double nx = 0, ny = 0;
        if (constrain_motion(e->delta_x, e->delta_y, e->unaccel_dx, e->unaccel_dy, e->time_msec, &nx, &ny))
            warp(nx, ny);
        server_events.push(PointerMotion{e->time_msec, cursor_x, cursor_y, e->unaccel_dx, e->unaccel_dy});
    });
    raw->motion_abs.connect(&p->events.motion_absolute, [this, raw](void* data) {
        auto* e = static_cast<wlr_pointer_motion_absolute_event*>(data);
        activity();
        wlr_box all{};
        wlr_output_layout_get_box(layout, raw->mapped_output, &all);
        double x = all.x + e->x * all.width, y = all.y + e->y * all.height;
        double nx = 0, ny = 0;
        if (constrain_motion(x - cursor_x, y - cursor_y, 0, 0, e->time_msec, &nx, &ny)) warp(nx, ny);
        server_events.push(PointerMotion{e->time_msec, cursor_x, cursor_y, 0, 0});
    });
    raw->button.connect(&p->events.button, [this](void* data) {
        auto* e = static_cast<wlr_pointer_button_event*>(data);
        activity();
        server_events.push(PointerButton{e->time_msec, e->button, e->state == WL_POINTER_BUTTON_STATE_PRESSED});
    });
    raw->axis.connect(&p->events.axis, [this](void* data) {
        auto* e = static_cast<wlr_pointer_axis_event*>(data);
        activity();
        server_events.push(PointerAxis{e->time_msec, uint32_t(e->orientation), uint32_t(e->source), e->delta,
                                       e->delta_discrete});
    });
    raw->frame.connect(&p->events.frame, [this](void*) { server_events.push(PointerFrame{}); });
    raw->destroy.connect(&p->base.events.destroy, [this, raw](void*) {
        pointers.remove_if([raw](const std::unique_ptr<PointerRec>& x) { return x.get() == raw; });
    });
    pointers.push_back(std::move(r));
    update_capabilities();
}

void Server::update_capabilities() {
    uint32_t caps = 0;
    if (!keyboards.empty()) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (!pointers.empty()) caps |= WL_SEAT_CAPABILITY_POINTER;
    if (!touches.empty()) caps |= WL_SEAT_CAPABILITY_TOUCH;
    wlr_seat_set_capabilities(seat, caps);
}

void Server::publish_cursor_position() {
    std::lock_guard<std::mutex> lock(mirror.m);
    mirror.cursor_x = cursor_x;
    mirror.cursor_y = cursor_y;
}

void Server::set_cursor(const CursorChanged& c) {
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        const CursorChanged& o = mirror.cursor;
        if (o.surface == c.surface && o.hotspot == c.hotspot && o.shape == c.shape && o.hidden == c.hidden) return;
        mirror.cursor = c;
    }
    server_events.push(c);
}

void Server::focus_surface(wlr_surface* surface) {
    // While locked the keyboard only ever reaches lock surfaces.
    if (surface && locked() && !lock_allows(surface)) return;
    wlr_keyboard* kb = wlr_seat_get_keyboard(seat);
    if (!surface) {
        wlr_seat_keyboard_notify_clear_focus(seat);
    } else {
        if (wlr_layer_surface_v1_try_from_wlr_surface(surface)) focused_layer = surface;
        // Enter with the routed modifier state (not the device's, which may be
        // ahead of keys the host has yet to forward).
        wlr_keyboard_modifiers wm{sent_modifiers.depressed, sent_modifiers.latched, sent_modifiers.locked,
                                  sent_modifiers.group};
        if (kb)
            wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes, &wm);
        else
            wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
    }
    wlr_surface* now = seat->keyboard_state.focused_surface;
    text_input_focus(now);
    update_shortcuts_inhibit();
    update_pad_focus(now);
}

void Server::focus_window(WindowId id) {
    WindowRef next = window_ref(id);
    WindowRef prev = window_ref(focused_window);
    if (prev && !(prev == next)) set_window_activated(prev, false);
    focused_window = next ? id : kNoWindow;
    if (!next) {
        if (!focused_layer && !locked()) focus_surface(nullptr);
        events.push(FocusChanged{kNoWindow});
        return;
    }
    set_window_activated(next, true);
    // An exclusive layer surface (launcher, a layer-shell locker) keeps the
    // keyboard; the session lock keeps it on the lock surface (the window
    // gets it back on unlock).
    bool layer_grab = false;
    if (focused_layer)
        if (auto* l = wlr_layer_surface_v1_try_from_wlr_surface(focused_layer))
            layer_grab = l->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
    if (!layer_grab && !locked()) {
        focused_layer = nullptr;
        focus_surface(window_surface(next));
    }
    events.push(FocusChanged{id});
}

void Server::forget_surface_focus(wlr_surface* surface) {
    if (focused_layer == surface) focused_layer = nullptr;
    for (auto& p : pads)
        if (p->focus == surface) p->focus = nullptr;  // wlroots drops the pad's focus with the surface
    if (pointer_surface == surface) {
        pointer_surface = nullptr;
        update_constraint();
    }
}

void Server::pointer_route(SurfaceId id, double sx, double sy, uint32_t time) {
    wlr_surface* target = id != kNoSurface ? surface_by_id(id) : nullptr;
    if (target && locked() && !lock_allows(target)) target = nullptr;
    if (!target) {
        if (pointer_surface || seat->pointer_state.focused_surface) {
            wlr_seat_pointer_notify_clear_focus(seat);
            CursorChanged c;
            c.shape = "default";
            set_cursor(c);
        }
        pointer_surface = nullptr;
        update_constraint();
        return;
    }
    // Remember where the surface's origin is in layout space (constraints
    // are surface-local).
    pointer_origin_x = cursor_x - sx;
    pointer_origin_y = cursor_y - sy;
    if (seat->pointer_state.focused_surface != target) {
        wlr_seat_pointer_notify_enter(seat, target, sx, sy);
        pointer_surface = target;
        update_constraint();
    }
    wlr_seat_pointer_notify_motion(seat, time ? time : now_msec(), sx, sy);
}

void Server::inject_key(uint32_t keycode, bool pressed) {
    wlr_keyboard_key_event e{};
    e.time_msec = now_msec();
    e.keycode = keycode;
    e.update_state = true;
    e.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(vkeyboard, &e);
}

void Server::inject_pointer_motion(double dx, double dy) {
    wlr_pointer_motion_event e{};
    e.pointer = vpointer;
    e.time_msec = now_msec();
    e.delta_x = e.unaccel_dx = dx;
    e.delta_y = e.unaccel_dy = dy;
    wl_signal_emit_mutable(&vpointer->events.motion, &e);
    wl_signal_emit_mutable(&vpointer->events.frame, vpointer);
}

void Server::inject_pointer_warp(double x, double y) {
    warp(x, y);
    server_events.push(PointerMotion{now_msec(), cursor_x, cursor_y, 0, 0});
    server_events.push(PointerFrame{});
}

void Server::inject_pointer_button(uint32_t button, bool pressed) {
    wlr_pointer_button_event e{};
    e.pointer = vpointer;
    e.time_msec = now_msec();
    e.button = button;
    e.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
    wl_signal_emit_mutable(&vpointer->events.button, &e);
    wl_signal_emit_mutable(&vpointer->events.frame, vpointer);
}

void Server::inject_pointer_axis(uint32_t orientation, double delta, int32_t discrete) {
    wlr_pointer_axis_event e{};
    e.pointer = vpointer;
    e.time_msec = now_msec();
    e.source = WL_POINTER_AXIS_SOURCE_WHEEL;
    e.orientation = wl_pointer_axis(orientation);
    e.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL;
    e.delta = delta;
    e.delta_discrete = discrete;
    wl_signal_emit_mutable(&vpointer->events.axis, &e);
    wl_signal_emit_mutable(&vpointer->events.frame, vpointer);
}

}  // namespace brocompositor::wl
