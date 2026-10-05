// Touch and tablets (tablet-unstable-v2): devices (libinput, nested, or the
// virtual ones the host injects into) report in layout space, the host
// hit-tests and routes back to surfaces. Tablet pads follow the keyboard
// focus, as the protocol expects.
#include "linux/server_impl.h"

#include <linux/input-event-codes.h>

#include <ctime>

namespace brocompositor::wl {

namespace {

uint32_t now_msec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
}

const wlr_touch_impl kVirtualTouchImpl = {"brocompositor-virtual-touch"};
const wlr_tablet_impl kVirtualTabletImpl = {"brocompositor-virtual-tablet"};
const wlr_tablet_pad_impl kVirtualPadImpl = {"brocompositor-virtual-pad"};

// Normalized device coordinates onto the device's output (when it names
// one), else onto the whole layout.
void to_layout(Server* s, const char* output_name, double nx, double ny, double* x, double* y) {
    wlr_output* o = nullptr;
    if (output_name)
        for (auto& [id, out] : s->outputs)
            if (out->output->name && std::string(out->output->name) == output_name) o = out->output;
    wlr_box b{};
    wlr_output_layout_get_box(s->layout, o, &b);
    *x = b.x + nx * b.width;
    *y = b.y + ny * b.height;
}

TabletToolType tool_type(wlr_tablet_tool_type t) { return TabletToolType(uint32_t(t)); }

}  // namespace

void Server::init_touch_tablet() { tablet_manager = wlr_tablet_v2_create(display); }

// ---------------------------------------------------------------- touch

void Server::add_touch(wlr_touch* touch, bool is_virtual) {
    auto r = std::make_unique<TouchRec>();
    TouchRec* t = r.get();
    t->srv = this;
    t->touch = touch;
    t->is_virtual = is_virtual;
    t->down.connect(&touch->events.down, [this, t](void* data) {
        auto* e = static_cast<wlr_touch_down_event*>(data);
        activity();
        TouchDown ev{e->time_msec, e->touch_id, 0, 0};
        to_layout(this, t->touch->output_name, e->x, e->y, &ev.x, &ev.y);
        server_events.push(ev);
    });
    t->motion.connect(&touch->events.motion, [this, t](void* data) {
        auto* e = static_cast<wlr_touch_motion_event*>(data);
        activity();
        TouchMotion ev{e->time_msec, e->touch_id, 0, 0};
        to_layout(this, t->touch->output_name, e->x, e->y, &ev.x, &ev.y);
        server_events.push(ev);
    });
    t->up.connect(&touch->events.up, [this](void* data) {
        auto* e = static_cast<wlr_touch_up_event*>(data);
        activity();
        server_events.push(TouchUp{e->time_msec, e->touch_id});
    });
    t->cancel.connect(&touch->events.cancel, [this](void* data) {
        auto* e = static_cast<wlr_touch_cancel_event*>(data);
        server_events.push(TouchCancel{e->time_msec, e->touch_id});
    });
    t->frame.connect(&touch->events.frame, [this](void*) { server_events.push(TouchFrame{}); });
    t->destroy.connect(&touch->base.events.destroy, [this, t](void*) {
        touches.remove_if([t](const std::unique_ptr<TouchRec>& p) { return p.get() == t; });
        update_capabilities();
    });
    touches.push_back(std::move(r));
    update_capabilities();
}

void Server::touch_down(SurfaceId id, int32_t touch_id, double sx, double sy, uint32_t time) {
    wlr_surface* target = id != kNoSurface ? surface_by_id(id) : nullptr;
    if (!target || (locked() && !lock_allows(target))) return;
    wlr_seat_touch_notify_down(seat, target, time ? time : now_msec(), touch_id, sx, sy);
}

void Server::touch_motion(int32_t touch_id, double sx, double sy, uint32_t time) {
    if (!wlr_seat_touch_get_point(seat, touch_id)) return;
    wlr_seat_touch_notify_motion(seat, time ? time : now_msec(), touch_id, sx, sy);
}

void Server::touch_up(int32_t touch_id, uint32_t time) {
    if (!wlr_seat_touch_get_point(seat, touch_id)) return;
    wlr_seat_touch_notify_up(seat, time ? time : now_msec(), touch_id);
}

void Server::touch_cancel() {
    std::set<wlr_seat_client*> clients;
    wlr_touch_point* p;
    wl_list_for_each(p, &seat->touch_state.touch_points, link) if (p->client) clients.insert(p->client);
    for (wlr_seat_client* c : clients) wlr_seat_touch_notify_cancel(seat, c);
}

void Server::inject_touch(int kind, int32_t id, double x, double y) {
    if (!vtouch) {
        vtouch = new wlr_touch{};
        wlr_touch_init(vtouch, &kVirtualTouchImpl, "brocompositor-virtual-touch");
        add_touch(vtouch, true);
    }
    wlr_box all{};
    wlr_output_layout_get_box(layout, nullptr, &all);
    double nx = all.width > 0 ? (x - all.x) / all.width : 0;
    double ny = all.height > 0 ? (y - all.y) / all.height : 0;
    uint32_t t = now_msec();
    switch (kind) {
        case kInjectDown: {
            wlr_touch_down_event e{vtouch, t, id, nx, ny};
            wl_signal_emit_mutable(&vtouch->events.down, &e);
            break;
        }
        case kInjectMotion: {
            wlr_touch_motion_event e{vtouch, t, id, nx, ny};
            wl_signal_emit_mutable(&vtouch->events.motion, &e);
            break;
        }
        case kInjectUp: {
            wlr_touch_up_event e{vtouch, t, id};
            wl_signal_emit_mutable(&vtouch->events.up, &e);
            break;
        }
        default: wl_signal_emit_mutable(&vtouch->events.frame, nullptr); break;
    }
}

// ---------------------------------------------------------------- tablets

TabletToolRec* Server::tool_rec(TabletToolId id) {
    for (auto& [k, t] : tablet_tools)
        if (t->id == id) return t.get();
    return nullptr;
}

void Server::add_tablet(wlr_tablet* tablet) {
    auto r = std::make_unique<TabletRec>();
    TabletRec* tr = r.get();
    tr->srv = this;
    tr->tablet = tablet;
    tr->v2 = wlr_tablet_create(tablet_manager, seat, &tablet->base);
    auto tool_of = [this, tr](wlr_tablet_tool* tool) -> TabletToolRec* {
        auto it = tablet_tools.find(tool);
        if (it == tablet_tools.end()) {
            auto t = std::make_unique<TabletToolRec>();
            t->id = next_tool++;
            t->tool = tool;
            t->v2 = wlr_tablet_tool_create(tablet_manager, seat, tool);
            TabletToolRec* raw = t.get();
            raw->destroy.connect(&tool->events.destroy, [this, tool](void*) { tablet_tools.erase(tool); });
            it = tablet_tools.emplace(tool, std::move(t)).first;
        }
        it->second->tablet = tr;
        return it->second.get();
    };
    tr->proximity.connect(&tablet->events.proximity, [this, tr, tool_of](void* data) {
        auto* e = static_cast<wlr_tablet_tool_proximity_event*>(data);
        activity();
        TabletToolRec* t = tool_of(e->tool);
        to_layout(this, nullptr, e->x, e->y, &t->x, &t->y);
        bool in = e->state == WLR_TABLET_TOOL_PROXIMITY_IN;
        server_events.push(TabletToolProximity{e->time_msec, t->id, tool_type(e->tool->type), in, t->x, t->y});
        (void)tr;
    });
    tr->axis.connect(&tablet->events.axis, [this, tool_of](void* data) {
        auto* e = static_cast<wlr_tablet_tool_axis_event*>(data);
        activity();
        TabletToolRec* t = tool_of(e->tool);
        wlr_box b{};
        wlr_output_layout_get_box(layout, nullptr, &b);
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_X) t->x = b.x + e->x * b.width;
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_Y) t->y = b.y + e->y * b.height;
        TabletToolMotion ev;
        ev.time_msec = e->time_msec;
        ev.tool = t->id;
        ev.x = t->x;
        ev.y = t->y;
        TabletToolAxes& a = ev.axes;
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) a.axes |= TabletToolAxes::Pressure, a.pressure = e->pressure;
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) a.axes |= TabletToolAxes::Distance, a.distance = e->distance;
        if (e->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
            a.axes |= TabletToolAxes::Tilt, a.tilt_x = e->tilt_x, a.tilt_y = e->tilt_y;
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) a.axes |= TabletToolAxes::Rotation, a.rotation = e->rotation;
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) a.axes |= TabletToolAxes::Slider, a.slider = e->slider;
        if (e->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) a.axes |= TabletToolAxes::Wheel, a.wheel_degrees = e->wheel_delta;
        server_events.push(ev);
    });
    tr->tip.connect(&tablet->events.tip, [this, tool_of](void* data) {
        auto* e = static_cast<wlr_tablet_tool_tip_event*>(data);
        activity();
        TabletToolRec* t = tool_of(e->tool);
        server_events.push(TabletToolTip{e->time_msec, t->id, e->state == WLR_TABLET_TOOL_TIP_DOWN});
    });
    tr->button.connect(&tablet->events.button, [this, tool_of](void* data) {
        auto* e = static_cast<wlr_tablet_tool_button_event*>(data);
        activity();
        TabletToolRec* t = tool_of(e->tool);
        server_events.push(TabletToolButton{e->time_msec, t->id, e->button, e->state == WLR_BUTTON_PRESSED});
    });
    tr->destroy.connect(&tablet->base.events.destroy, [this, tr](void*) {
        for (auto& [k, t] : tablet_tools)
            if (t->tablet == tr) t->tablet = nullptr;
        for (auto& p : pads)
            if (p->tablet == tr) p->tablet = nullptr;
        tablets.remove_if([tr](const std::unique_ptr<TabletRec>& p) { return p.get() == tr; });
    });
    tablets.push_back(std::move(r));
}

void Server::add_tablet_pad(wlr_tablet_pad* pad) {
    auto r = std::make_unique<TabletPadRec>();
    TabletPadRec* p = r.get();
    p->srv = this;
    p->id = next_pad++;
    p->pad = pad;
    p->v2 = wlr_tablet_pad_create(tablet_manager, seat, &pad->base);
    if (!tablets.empty()) p->tablet = tablets.back().get();
    p->button.connect(&pad->events.button, [this, p](void* data) {
        auto* e = static_cast<wlr_tablet_pad_button_event*>(data);
        activity();
        server_events.push(TabletPadButton{e->time_msec, p->id, e->button, e->state == WLR_BUTTON_PRESSED});
    });
    p->ring.connect(&pad->events.ring, [this, p](void* data) {
        auto* e = static_cast<wlr_tablet_pad_ring_event*>(data);
        activity();
        server_events.push(TabletPadRing{e->time_msec, p->id, e->ring, e->position,
                                         e->source == WLR_TABLET_PAD_RING_SOURCE_FINGER});
    });
    p->strip.connect(&pad->events.strip, [this, p](void* data) {
        auto* e = static_cast<wlr_tablet_pad_strip_event*>(data);
        activity();
        server_events.push(TabletPadStrip{e->time_msec, p->id, e->strip, e->position,
                                          e->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER});
    });
    p->attach.connect(&pad->events.attach_tablet, [this, p](void* data) {
        auto* tool = static_cast<wlr_tablet_tool*>(data);
        auto it = tablet_tools.find(tool);
        if (it != tablet_tools.end() && it->second->tablet) p->tablet = it->second->tablet;
    });
    p->destroy.connect(&pad->base.events.destroy, [this, p](void*) {
        pads.remove_if([p](const std::unique_ptr<TabletPadRec>& x) { return x.get() == p; });
    });
    pads.push_back(std::move(r));
    update_pad_focus(seat->keyboard_state.focused_surface);
}

void Server::tablet_tool_route(TabletToolId id, SurfaceId surface, double sx, double sy, const TabletToolAxes& a) {
    TabletToolRec* t = tool_rec(id);
    if (!t || !t->v2) return;
    wlr_surface* target = surface != kNoSurface ? surface_by_id(surface) : nullptr;
    if (target && locked() && !lock_allows(target)) target = nullptr;
    if (target && (!t->tablet || !t->tablet->v2 || !wlr_surface_accepts_tablet_v2(t->tablet->v2, target)))
        target = nullptr;
    if (t->v2->focused_surface != target) {
        if (t->v2->focused_surface) wlr_tablet_v2_tablet_tool_notify_proximity_out(t->v2);
        if (target) wlr_tablet_v2_tablet_tool_notify_proximity_in(t->v2, t->tablet->v2, target);
    }
    if (!target) return;
    wlr_tablet_v2_tablet_tool_notify_motion(t->v2, sx, sy);
    if (a.axes & TabletToolAxes::Pressure) wlr_tablet_v2_tablet_tool_notify_pressure(t->v2, a.pressure);
    if (a.axes & TabletToolAxes::Distance) wlr_tablet_v2_tablet_tool_notify_distance(t->v2, a.distance);
    if (a.axes & TabletToolAxes::Tilt) wlr_tablet_v2_tablet_tool_notify_tilt(t->v2, a.tilt_x, a.tilt_y);
    if (a.axes & TabletToolAxes::Rotation) wlr_tablet_v2_tablet_tool_notify_rotation(t->v2, a.rotation);
    if (a.axes & TabletToolAxes::Slider) wlr_tablet_v2_tablet_tool_notify_slider(t->v2, a.slider);
    if (a.axes & TabletToolAxes::Wheel)
        wlr_tablet_v2_tablet_tool_notify_wheel(t->v2, a.wheel_degrees, a.wheel_clicks);
}

void Server::tablet_tool_tip(TabletToolId id, bool down) {
    TabletToolRec* t = tool_rec(id);
    if (!t || !t->v2 || !t->v2->focused_surface) return;
    if (down) wlr_tablet_v2_tablet_tool_notify_down(t->v2);
    else wlr_tablet_v2_tablet_tool_notify_up(t->v2);
}

void Server::tablet_tool_button(TabletToolId id, uint32_t button, bool pressed) {
    TabletToolRec* t = tool_rec(id);
    if (!t || !t->v2 || !t->v2->focused_surface) return;
    wlr_tablet_v2_tablet_tool_notify_button(
        t->v2, button, pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

void Server::tablet_pad_event(TabletPadId id, int kind, uint32_t time, uint32_t index, double position, bool on) {
    TabletPadRec* p = nullptr;
    for (auto& x : pads)
        if (x->id == id) p = x.get();
    if (!p || !p->v2 || !p->focus) return;
    if (kind == kPadButton)
        wlr_tablet_v2_tablet_pad_notify_button(
            p->v2, index, time, on ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
    else if (kind == kPadRing)
        wlr_tablet_v2_tablet_pad_notify_ring(p->v2, index, position, on, time);
    else
        wlr_tablet_v2_tablet_pad_notify_strip(p->v2, index, position, on, time);
}

void Server::update_pad_focus(wlr_surface* focus) {
    for (auto& p : pads) {
        if (!p->v2 || p->focus == focus) continue;
        if (p->focus) wlr_tablet_v2_tablet_pad_notify_leave(p->v2, p->focus);
        p->focus = nullptr;
        if (focus && p->tablet && p->tablet->v2 && wlr_surface_accepts_tablet_v2(p->tablet->v2, focus)) {
            wlr_tablet_v2_tablet_pad_notify_enter(p->v2, p->tablet->v2, focus);
            p->focus = focus;
        }
    }
}

void Server::inject_tablet(int kind, double x, double y, double pressure, uint32_t button, bool on) {
    if (!vtablet) {
        vtablet = new wlr_tablet{};
        wlr_tablet_init(vtablet, &kVirtualTabletImpl, "brocompositor-virtual-tablet");
        vtablet->width_mm = 300;
        vtablet->height_mm = 200;
        add_tablet(vtablet);
        vtool = new wlr_tablet_tool{};
        vtool->type = WLR_TABLET_TOOL_TYPE_PEN;
        vtool->hardware_serial = 0xb0c0;
        vtool->pressure = true;
        wl_signal_init(&vtool->events.destroy);
        vpad = new wlr_tablet_pad{};
        wlr_tablet_pad_init(vpad, &kVirtualPadImpl, "brocompositor-virtual-pad");
        vpad->button_count = 4;
        auto* group = new wlr_tablet_pad_group{};
        static unsigned int buttons[4] = {0, 1, 2, 3};
        group->button_count = 4;
        group->buttons = buttons;
        group->mode_count = 1;
        wl_list_insert(&vpad->groups, &group->link);
        add_tablet_pad(vpad);
    }
    wlr_box all{};
    wlr_output_layout_get_box(layout, nullptr, &all);
    if (kind == kInjectProximity || kind == kInjectToolMotion) {
        vtool_x = all.width > 0 ? (x - all.x) / all.width : 0;
        vtool_y = all.height > 0 ? (y - all.y) / all.height : 0;
    }
    uint32_t t = now_msec();
    switch (kind) {
        case kInjectProximity: {
            wlr_tablet_tool_proximity_event e{vtablet, vtool, t, vtool_x, vtool_y,
                                              on ? WLR_TABLET_TOOL_PROXIMITY_IN : WLR_TABLET_TOOL_PROXIMITY_OUT};
            wl_signal_emit_mutable(&vtablet->events.proximity, &e);
            break;
        }
        case kInjectToolMotion: {
            wlr_tablet_tool_axis_event e{};
            e.tablet = vtablet;
            e.tool = vtool;
            e.time_msec = t;
            e.updated_axes = WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y | WLR_TABLET_TOOL_AXIS_PRESSURE;
            e.x = vtool_x;
            e.y = vtool_y;
            e.pressure = pressure;
            wl_signal_emit_mutable(&vtablet->events.axis, &e);
            break;
        }
        case kInjectTip: {
            wlr_tablet_tool_tip_event e{vtablet, vtool, t, vtool_x, vtool_y,
                                        on ? WLR_TABLET_TOOL_TIP_DOWN : WLR_TABLET_TOOL_TIP_UP};
            wl_signal_emit_mutable(&vtablet->events.tip, &e);
            break;
        }
        case kInjectToolButton: {
            wlr_tablet_tool_button_event e{vtablet, vtool, t, button,
                                           on ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED};
            wl_signal_emit_mutable(&vtablet->events.button, &e);
            break;
        }
        default: {
            wlr_tablet_pad_button_event e{t, button, on ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED, 0, 0};
            wl_signal_emit_mutable(&vpad->events.button, &e);
            break;
        }
    }
}

void Server::shutdown_touch_tablet() {
    if (vpad) {
        wlr_tablet_pad_group* g;
        wlr_tablet_pad_group* tmp;
        wl_list_for_each_safe(g, tmp, &vpad->groups, link) {
            wl_list_remove(&g->link);
            delete g;
        }
        wlr_tablet_pad_finish(vpad);
        delete vpad;
        vpad = nullptr;
    }
    if (vtool) {
        wl_signal_emit_mutable(&vtool->events.destroy, vtool);
        delete vtool;
        vtool = nullptr;
    }
    if (vtablet) {
        wlr_tablet_finish(vtablet);
        delete vtablet;
        vtablet = nullptr;
    }
    if (vtouch) {
        wlr_touch_finish(vtouch);
        delete vtouch;
        vtouch = nullptr;
    }
    pads.clear();
    tablet_tools.clear();
    tablets.clear();
    touches.clear();
}

}  // namespace brocompositor::wl
