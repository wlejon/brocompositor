// Idle: ext-idle-notify (swayidle and friends get idled / resumed from the
// seat's input activity) and idle-inhibit (a visible surface, e.g. a playing
// video, keeps the session from idling).
#include "linux/server_impl.h"

namespace brocompositor::wl {

void Server::init_idle() {
    idle_notifier = wlr_idle_notifier_v1_create(display);
    idle_inhibit = wlr_idle_inhibit_v1_create(display);
    new_idle_inhibitor.connect(&idle_inhibit->events.new_inhibitor, [this](void* data) {
        auto* in = static_cast<wlr_idle_inhibitor_v1*>(data);
        auto rec = std::make_unique<IdleInhibitorRec>();
        rec->inhibitor = in;
        rec->destroy.connect(&in->events.destroy, [this, in](void*) {
            idle_inhibitors.erase(in);
            schedule_idle_check();
        });
        idle_inhibitors[in] = std::move(rec);
        schedule_idle_check();
    });
}

void Server::activity() {
    if (idle_notifier) wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
}

void Server::schedule_idle_check() {
    if (idle_check || !loop) return;
    idle_check = wl_event_loop_add_idle(
        loop,
        [](void* data) {
            auto* s = static_cast<Server*>(data);
            s->idle_check = nullptr;
            s->update_idle_inhibit();
        },
        this);
}

// A surface counts as visible when its tree is mapped and the host shows it:
// a visible window, a mapped layer surface or unmanaged surface, or (only
// those while locked) a lock surface.
bool Server::surface_visible(wlr_surface* surface) {
    if (!surface || !surface->mapped) return false;
    RootRef r = resolve_root(surface);
    if (r.lock) return true;
    if (locked()) return false;
    if (r.top) return r.top->id != kNoWindow && r.top->visible;
    if (r.x) return (r.x->id != kNoWindow && r.x->visible) || r.x->unmanaged != 0;
    if (r.layer) return r.layer->mapped;
    return false;
}

void Server::update_idle_inhibit() {
    bool inhibited = false;
    for (auto& [in, rec] : idle_inhibitors)
        if (surface_visible(in->surface)) inhibited = true;
    if (inhibited == idle_inhibited) return;
    idle_inhibited = inhibited;
    if (idle_notifier) wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
    {
        std::lock_guard<std::mutex> lock(mirror.m);
        mirror.idle_inhibited = inhibited;
    }
    server_events.push(IdleInhibitChanged{inhibited});
}

}  // namespace brocompositor::wl
