// ext-session-lock-v1. The lock is enforced here, not by the host's good
// behaviour: once a client locks the session, every non-lock surface is
// withheld from the host (empty trees, refused acquire()), loses keyboard,
// pointer, touch and tablet focus, cannot regain it (focus and routing
// refuse it, seat grabs are ended, the input-method grab is bypassed), and
// gets no frame callbacks. `locked` goes to the lock client only after every
// enabled output has presented a frame drawn without client content.
//
// If the lock client dies without unlocking the session stays locked
// (Abandoned); only a new lock client can unlock it. There is no host-side
// unlock.
#include "linux/server_impl.h"

namespace brocompositor::wl {

void Server::set_lock_state(LockState state) {
    if (state == lock_state) return;
    lock_state = state;
    lock_gate->locked.store(state != LockState::Unlocked, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.lock_state = state;
    }
    server_events.push(SessionLockChanged{state});
}

void Server::end_seat_grabs() {
    wlr_seat_pointer_end_grab(seat);
    wlr_seat_keyboard_end_grab(seat);
    wlr_seat_touch_end_grab(seat);
}

void Server::init_session_lock() {
    lock_manager = wlr_session_lock_manager_v1_create(display);

    new_lock.connect(&lock_manager->events.new_lock, [this](void* data) {
        auto* l = static_cast<wlr_session_lock_v1*>(data);
        if (lock) {
            // Another client holds the lock: this one is refused (finished).
            wlr_session_lock_v1_destroy(l);
            return;
        }
        bool takeover = lock_state == LockState::Abandoned;
        lock = l;
        lock_new_surface.connect(&l->events.new_surface, [this](void* data) {
            auto* ls = static_cast<wlr_session_lock_surface_v1*>(data);
            auto rec = std::make_unique<LockSurfaceRec>();
            LockSurfaceRec* r = rec.get();
            r->ls = ls;
            OutputRec* o = output_rec(ls->output);
            r->output = o ? o->id : kNoMonitor;
            r->map.connect(&ls->surface->events.map, [this](void*) {
                refresh_lock_trees();
                focus_lock_surface();
            });
            r->unmap.connect(&ls->surface->events.unmap, [this, ls](void*) {
                if (seat->keyboard_state.focused_surface == ls->surface) focus_surface(nullptr);
                refresh_lock_trees();
                focus_lock_surface();
            });
            r->destroy.connect(&ls->events.destroy, [this, ls](void*) {
                lock_surfaces.erase(ls);
                refresh_lock_trees();
                if (locked()) focus_lock_surface();
            });
            lock_surfaces[ls] = std::move(rec);
            configure_lock_surfaces();
        });
        lock_unlock.connect(&l->events.unlock, [this](void*) {
            lock = nullptr;
            lock_new_surface.disconnect();
            lock_unlock.disconnect();
            lock_destroy.disconnect();
            lock_waiting.clear();
            set_lock_state(LockState::Unlocked);
            {
                std::lock_guard<std::mutex> g(lock_gate->m);
                lock_gate->allowed.clear();
            }
            refresh_lock_trees();
            // The keyboard goes back to where it was before the lock.
            focus_surface(nullptr);
            if (focused_layer)
                focus_surface(focused_layer);
            else if (focused_window != kNoWindow)
                focus_window(focused_window);
            mark_all_trees_dirty();
            schedule_idle_check();
            for (auto& [id, out] : outputs)
                if (out->output->enabled) wlr_output_schedule_frame(out->output);
        });
        lock_destroy.connect(&l->events.destroy, [this](void*) {
            // Destroyed without unlocking: the client died or gave up.
            lock = nullptr;
            lock_new_surface.disconnect();
            lock_unlock.disconnect();
            lock_destroy.disconnect();
            set_lock_state(LockState::Abandoned);
        });

        if (takeover) {
            // Nothing but the host's lock background is on screen already.
            set_lock_state(LockState::Locked);
            wlr_session_lock_v1_send_locked(l);
            return;
        }
        set_lock_state(LockState::Locked);
        {
            std::lock_guard<std::mutex> g(lock_gate->m);
            lock_gate->allowed.clear();
        }
        // Cut every non-lock surface off from input.
        end_seat_grabs();
        focus_surface(nullptr);
        wlr_seat_pointer_notify_clear_focus(seat);
        pointer_surface = nullptr;
        update_constraint();
        touch_cancel();
        for (auto& [k, t] : tablet_tools)
            if (t->v2 && t->v2->focused_surface) wlr_tablet_v2_tablet_tool_notify_proximity_out(t->v2);
        refresh_lock_trees();
        schedule_idle_check();
        lock_waiting.clear();
        for (auto& [id, out] : outputs)
            if (out->output->enabled) {
                lock_waiting.insert(id);
                wlr_output_schedule_frame(out->output);
            }
        if (lock_waiting.empty()) wlr_session_lock_v1_send_locked(l);
    });

    // A grab begun while locked (a popup grab from an input event that
    // predates the lock, a drag) is ended as soon as the begin returns.
    auto block_grab = [this](void*) {
        if (!locked() || grab_end_idle) return;
        grab_end_idle = wl_event_loop_add_idle(
            loop,
            [](void* data) {
                auto* s = static_cast<Server*>(data);
                s->grab_end_idle = nullptr;
                if (s->locked()) s->end_seat_grabs();
            },
            this);
    };
    pointer_grab_begin.connect(&seat->events.pointer_grab_begin, block_grab);
    keyboard_grab_begin.connect(&seat->events.keyboard_grab_begin, block_grab);
    touch_grab_begin.connect(&seat->events.touch_grab_begin, block_grab);
}

bool Server::lock_allows(wlr_surface* surface) {
    return surface && resolve_root(surface).lock != nullptr;
}

void Server::lock_output_presented(MonitorId output) {
    if (!lock || lock_state != LockState::Locked || lock_waiting.empty()) return;
    lock_waiting.erase(output);
    if (lock_waiting.empty() && !lock->locked_sent) wlr_session_lock_v1_send_locked(lock);
}

void Server::refresh_lock_trees() {
    std::map<MonitorId, std::vector<SurfaceNode>> trees;
    std::set<SurfaceId> allowed;
    for (auto& [ls, r] : lock_surfaces) {
        if (!ls->surface->mapped || r->output == kNoMonitor) continue;
        auto tree = build_tree(ls->surface, Point{0, 0}, nullptr, nullptr);
        for (const SurfaceNode& n : tree) allowed.insert(n.surface);
        trees[r->output] = std::move(tree);
    }
    {
        std::lock_guard<std::mutex> g(lock_gate->m);
        lock_gate->allowed = allowed;
    }
    std::vector<LockSurfaceChanged> changes;
    {
        std::lock_guard<std::mutex> g(mirror.m);
        for (auto& [out, tree] : mirror.lock_trees)
            if (!trees.count(out)) changes.push_back(LockSurfaceChanged{out, kNoSurface});
        for (auto& [out, tree] : trees) {
            auto it = mirror.lock_trees.find(out);
            if (it == mirror.lock_trees.end() || it->second != tree)
                changes.push_back(LockSurfaceChanged{out, tree.empty() ? kNoSurface : tree.front().surface});
        }
        mirror.lock_trees = std::move(trees);
    }
    for (auto& c : changes) server_events.push(c);
}

void Server::configure_lock_surfaces() {
    for (auto& [ls, r] : lock_surfaces) {
        OutputRec* o = output_rec(ls->output);
        r->output = o ? o->id : kNoMonitor;
        if (!o) continue;
        int w = 0, h = 0;
        wlr_output_effective_resolution(ls->output, &w, &h);
        if (w <= 0 || h <= 0) continue;
        if (ls->configured && ls->current.width == uint32_t(w) && ls->current.height == uint32_t(h)) continue;
        bool pending = false;
        // (the struct shares its name with the configure function)
        struct wlr_session_lock_surface_v1_configure* c;
        wl_list_for_each(c, &ls->configure_list, link) {
            if (c->width == uint32_t(w) && c->height == uint32_t(h)) pending = true;
        }
        if (!pending) wlr_session_lock_surface_v1_configure(ls, uint32_t(w), uint32_t(h));
    }
}

// The keyboard goes to the lock surface on the output under the cursor (the
// first mapped one otherwise), unless it already is on a lock surface.
void Server::focus_lock_surface() {
    if (!locked()) return;
    wlr_surface* cur = seat->keyboard_state.focused_surface;
    if (cur && lock_allows(cur)) return;
    wlr_output* under = output_at(cursor_x, cursor_y);
    wlr_surface* pick = nullptr;
    for (auto& [ls, r] : lock_surfaces) {
        if (!ls->surface->mapped) continue;
        if (ls->output == under) {
            pick = ls->surface;
            break;
        }
        if (!pick) pick = ls->surface;
    }
    if (pick) focus_surface(pick);
}

}  // namespace brocompositor::wl
