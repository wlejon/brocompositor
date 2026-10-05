// Text input relay: text-input-v3 clients (the surface being typed into) <->
// one input-method-v2 client (the IME). The relay follows the keyboard
// focus: the focused client's text inputs get enter/leave, the enabled one
// activates the IME, IME commits (preedit, commit string, delete) go back to
// it, and the IME's keyboard grab sees the keys the host routes. IME popup
// surfaces (candidate windows) join the focused window's surface tree,
// below the text cursor rectangle.
#include "linux/server_impl.h"

namespace brocompositor::wl {

namespace {

void send_im_state(wlr_input_method_v2* im, wlr_text_input_v3* ti) {
    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
        wlr_input_method_v2_send_surrounding_text(im, ti->current.surrounding.text ? ti->current.surrounding.text : "",
                                                  ti->current.surrounding.cursor, ti->current.surrounding.anchor);
    wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
        wlr_input_method_v2_send_content_type(im, ti->current.content_type.hint, ti->current.content_type.purpose);
    wlr_input_method_v2_send_done(im);
}

wl_client* client_of(wlr_surface* s) { return s ? wl_resource_get_client(s->resource) : nullptr; }

}  // namespace

wlr_text_input_v3* Server::active_text_input() {
    if (!text_focus) return nullptr;
    for (auto& [ti, rec] : text_inputs)
        if (ti->current_enabled && ti->focused_surface == text_focus) return ti;
    return nullptr;
}

void Server::init_text_input() {
    text_input_manager = wlr_text_input_manager_v3_create(display);
    input_method_manager = wlr_input_method_manager_v2_create(display);

    new_text_input.connect(&text_input_manager->events.text_input, [this](void* data) {
        auto* ti = static_cast<wlr_text_input_v3*>(data);
        auto rec = std::make_unique<TextInputRec>();
        TextInputRec* r = rec.get();
        r->ti = ti;
        r->enable.connect(&ti->events.enable, [this, ti](void*) {
            if (!input_method || ti != active_text_input() || !input_method_allowed()) return;
            wlr_input_method_v2_send_activate(input_method);
            send_im_state(input_method, ti);
        });
        r->commit.connect(&ti->events.commit, [this, ti](void*) {
            if (ti != active_text_input()) return;
            // A disallowed input method does not even see the text (a lock
            // screen's password field).
            if (input_method && input_method_allowed()) send_im_state(input_method, ti);
            // The cursor rectangle moves the IME popups.
            for (auto& [p, pr] : input_popups) {
                wlr_box b = ti->current.cursor_rectangle;
                wlr_box rel{0, -b.height, b.width, b.height};
                wlr_input_popup_surface_v2_send_text_input_rectangle(p, &rel);
            }
            if (ti->focused_surface) mark_tree_dirty(ti->focused_surface);
        });
        r->disable.connect(&ti->events.disable, [this, ti](void*) {
            if (!input_method || ti->focused_surface != text_focus) return;
            wlr_input_method_v2_send_deactivate(input_method);
            wlr_input_method_v2_send_done(input_method);
        });
        r->destroy.connect(&ti->events.destroy, [this, ti](void*) {
            bool was_active = ti == active_text_input();
            text_inputs.erase(ti);
            if (was_active && input_method) {
                wlr_input_method_v2_send_deactivate(input_method);
                wlr_input_method_v2_send_done(input_method);
            }
        });
        text_inputs[ti] = std::move(rec);
        if (text_focus && client_of(text_focus) == wl_resource_get_client(ti->resource))
            wlr_text_input_v3_send_enter(ti, text_focus);
    });

    new_input_method.connect(&input_method_manager->events.input_method, [this](void* data) {
        auto* im = static_cast<wlr_input_method_v2*>(data);
        if (input_method) {
            // One input method per seat.
            wlr_input_method_v2_send_unavailable(im);
            return;
        }
        input_method = im;
        im_commit.connect(&im->events.commit, [this, im](void*) {
            wlr_text_input_v3* ti = active_text_input();
            // While locked an input method types into nothing unless allowed.
            if (!ti || !input_method_allowed()) return;
            if (im->current.preedit.text)
                wlr_text_input_v3_send_preedit_string(ti, im->current.preedit.text, im->current.preedit.cursor_begin,
                                                      im->current.preedit.cursor_end);
            else
                wlr_text_input_v3_send_preedit_string(ti, nullptr, 0, 0);
            if (im->current.commit_text) wlr_text_input_v3_send_commit_string(ti, im->current.commit_text);
            if (im->current.delete_.before_length || im->current.delete_.after_length)
                wlr_text_input_v3_send_delete_surrounding_text(ti, im->current.delete_.before_length,
                                                               im->current.delete_.after_length);
            wlr_text_input_v3_send_done(ti);
        });
        im_new_popup.connect(&im->events.new_popup_surface, [this](void* data) {
            auto* popup = static_cast<wlr_input_popup_surface_v2*>(data);
            auto rec = std::make_unique<InputPopupRec>();
            InputPopupRec* r = rec.get();
            r->popup = popup;
            r->map.connect(&popup->surface->events.map, [this, popup](void*) {
                if (wlr_text_input_v3* ti = active_text_input()) {
                    wlr_box b = ti->current.cursor_rectangle;
                    wlr_box rel{0, -b.height, b.width, b.height};
                    wlr_input_popup_surface_v2_send_text_input_rectangle(popup, &rel);
                }
                mark_all_trees_dirty();
            });
            r->unmap.connect(&popup->surface->events.unmap, [this](void*) { mark_all_trees_dirty(); });
            r->destroy.connect(&popup->events.destroy, [this, popup](void*) {
                input_popups.erase(popup);
                mark_all_trees_dirty();
            });
            input_popups[popup] = std::move(rec);
        });
        im_grab_keyboard.connect(&im->events.grab_keyboard, [this](void* data) {
            auto* grab = static_cast<wlr_input_method_keyboard_grab_v2*>(data);
            wlr_input_method_keyboard_grab_v2_set_keyboard(grab, wlr_seat_get_keyboard(seat));
            im_grab_destroy.connect(&grab->events.destroy, [this](void*) {
                im_grab_destroy.disconnect();
                // Keys pressed during the grab are released to the client.
                if (wlr_keyboard* kb = wlr_seat_get_keyboard(seat)) {
                    wlr_seat_keyboard_notify_modifiers(seat, &kb->modifiers);
                }
            });
        });
        im_destroy.connect(&im->events.destroy, [this](void*) {
            input_method = nullptr;
            im_commit.disconnect();
            im_new_popup.disconnect();
            im_grab_keyboard.disconnect();
            im_grab_destroy.disconnect();
            im_destroy.disconnect();
            if (wlr_text_input_v3* ti = active_text_input()) {
                wlr_text_input_v3_send_preedit_string(ti, nullptr, 0, 0);
                wlr_text_input_v3_send_done(ti);
            }
        });
        if (wlr_text_input_v3* ti = active_text_input(); ti && input_method_allowed()) {
            wlr_input_method_v2_send_activate(im);
            send_im_state(im, ti);
        }
    });
}

bool Server::input_method_allowed() const {
    return !input_method || client_input_allowed(client_pid(wl_resource_get_client(input_method->resource)));
}

void Server::text_input_focus(wlr_surface* focus) {
    if (focus == text_focus) return;
    for (auto& [ti, rec] : text_inputs) {
        if (!ti->focused_surface || ti->focused_surface == focus) continue;
        if (ti->current_enabled && ti->focused_surface == text_focus && input_method) {
            wlr_input_method_v2_send_deactivate(input_method);
            wlr_input_method_v2_send_done(input_method);
        }
        wlr_text_input_v3_send_leave(ti);
    }
    text_focus = focus;
    if (!focus) return;
    for (auto& [ti, rec] : text_inputs)
        if (ti->focused_surface != focus && wl_resource_get_client(ti->resource) == client_of(focus))
            wlr_text_input_v3_send_enter(ti, focus);
}

bool Server::im_grab_key(uint32_t time, uint32_t key, bool pressed) {
    if (!input_method || !input_method->keyboard_grab || locked()) return false;
    wlr_keyboard* kb = wlr_seat_get_keyboard(seat);
    // Keys the input method typed itself (its virtual keyboard) go on to the
    // client, or they would loop back into the IME.
    wl_client* im_client = wl_resource_get_client(input_method->resource);
    for (auto& k : keyboards)
        if (k->keyboard == kb && k->owner == im_client) return false;
    wlr_input_method_keyboard_grab_v2* grab = input_method->keyboard_grab;
    if (kb && grab->keyboard != kb) wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
    wlr_input_method_keyboard_grab_v2_send_key(grab, time, key,
                                               pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    return true;
}

bool Server::im_grab_modifiers(const KeyboardModifiers& m) {
    if (!input_method || !input_method->keyboard_grab || locked()) return false;
    wlr_keyboard* kb = wlr_seat_get_keyboard(seat);
    wl_client* im_client = wl_resource_get_client(input_method->resource);
    for (auto& k : keyboards)
        if (k->keyboard == kb && k->owner == im_client) return false;
    wlr_keyboard_modifiers wm{m.depressed, m.latched, m.locked, m.group};
    wlr_input_method_keyboard_grab_v2_send_modifiers(input_method->keyboard_grab, &wm);
    return true;
}

void Server::append_im_popups(WindowId window, std::vector<SurfaceNode>& tree) {
    if (input_popups.empty()) return;
    wlr_text_input_v3* ti = active_text_input();
    if (!ti || !ti->focused_surface || resolve_root(ti->focused_surface).window() != window) return;
    SurfaceId focus_id = surface_id(ti->focused_surface);
    Point base;
    bool found = false;
    for (const SurfaceNode& n : tree)
        if (n.surface == focus_id) {
            base = n.offset;
            found = true;
        }
    if (!found) return;
    const wlr_box& b = ti->current.cursor_rectangle;
    for (auto& [popup, rec] : input_popups) {
        wlr_surface* s = popup->surface;
        if (!s->mapped) continue;
        SurfaceNode n;
        n.surface = surface_id(s);
        n.offset = Point{base.x + b.x, base.y + b.y + b.height};
        n.size = Size{s->current.width, s->current.height};
        n.popup = true;
        tree.push_back(n);
    }
}

}  // namespace brocompositor::wl
