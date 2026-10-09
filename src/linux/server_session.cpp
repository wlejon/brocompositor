// ServerBackend facade for the desktop-session features: XWayland, touch,
// tablets, idle, session lock, capture and gamma.
#include "brocompositor/linux/server.h"

#include "linux/server_impl.h"

#include <unistd.h>

#include <cstring>

namespace brocompositor::wl {

struct ServerBackend::Impl : Server {};

bool ServerBackend::set_window_minimized(WindowId id, bool minimized) {
    Server* s = impl_.get();
    return s->dispatcher->call([=] { return s->set_window_minimized(id, minimized); });
}

std::string ServerBackend::create_activation_token(const std::string& app_id) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, app_id]() -> std::string {
        if (!s->activation) return {};
        // No seat or serial: wlroots then takes the token on its name alone,
        // which is what a launch by the host itself wants.
        wlr_xdg_activation_token_v1* t = wlr_xdg_activation_token_v1_create(s->activation);
        if (!t) return {};
        if (!app_id.empty()) t->app_id = strdup(app_id.c_str());
        const char* name = wlr_xdg_activation_token_v1_get_name(t);
        return name ? std::string(name) : std::string();
    });
}

std::string ServerBackend::xwayland_display() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.xwayland_display;
}

std::vector<UnmanagedSurfaceInfo> ServerBackend::unmanaged_surfaces() const {
    if (impl_->lock_gate->locked) return {};
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.unmanaged;
}

// ---------------------------------------------------------------- touch

void ServerBackend::touch_down(SurfaceId surface, int32_t id, double sx, double sy, uint32_t time_msec) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->touch_down(surface, id, sx, sy, time_msec); });
}

void ServerBackend::touch_motion(int32_t id, double sx, double sy, uint32_t time_msec) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->touch_motion(id, sx, sy, time_msec); });
}

void ServerBackend::touch_up(int32_t id, uint32_t time_msec) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->touch_up(id, time_msec); });
}

void ServerBackend::touch_cancel() {
    Server* s = impl_.get();
    s->dispatcher->post([s] { s->touch_cancel(); });
}

void ServerBackend::touch_frame() {
    Server* s = impl_.get();
    s->dispatcher->post([s] { wlr_seat_touch_notify_frame(s->seat); });
}

// ---------------------------------------------------------------- tablets

void ServerBackend::tablet_tool_route(TabletToolId tool, SurfaceId surface, double sx, double sy,
                                      const TabletToolAxes& axes) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->tablet_tool_route(tool, surface, sx, sy, axes); });
}

void ServerBackend::tablet_tool_tip(TabletToolId tool, bool down) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->tablet_tool_tip(tool, down); });
}

void ServerBackend::tablet_tool_button(TabletToolId tool, uint32_t button, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->tablet_tool_button(tool, button, pressed); });
}

void ServerBackend::tablet_pad_button(TabletPadId pad, uint32_t time_msec, uint32_t button, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->tablet_pad_event(pad, kPadButton, time_msec, button, 0, pressed); });
}

void ServerBackend::tablet_pad_ring(TabletPadId pad, uint32_t time_msec, uint32_t ring, double position,
                                   bool finger) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->tablet_pad_event(pad, kPadRing, time_msec, ring, position, finger); });
}

void ServerBackend::tablet_pad_strip(TabletPadId pad, uint32_t time_msec, uint32_t strip, double position,
                                    bool finger) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->tablet_pad_event(pad, kPadStrip, time_msec, strip, position, finger); });
}

// ---------------------------------------------------------------- idle

void ServerBackend::notify_activity() {
    Server* s = impl_.get();
    s->dispatcher->post([s] { s->activity(); });
}

bool ServerBackend::idle_inhibited() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.idle_inhibited;
}

// ---------------------------------------------------------------- session lock

LockState ServerBackend::session_lock_state() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.lock_state;
}

std::vector<SurfaceNode> ServerBackend::lock_surface_tree(MonitorId output) const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.lock_trees.find(output);
    if (it == impl_->mirror.lock_trees.end()) return {};
    return it->second;
}

std::optional<SurfaceHit> ServerBackend::hit_test_lock(MonitorId output, double ox, double oy) const {
    Server* s = impl_.get();
    return s->dispatcher->call([=]() -> std::optional<SurfaceHit> {
        for (auto& [ls, rec] : s->lock_surfaces) {
            if (rec->output != output || !ls->surface->mapped) continue;
            double sx = 0, sy = 0;
            wlr_surface* hit = wlr_surface_surface_at(ls->surface, ox, oy, &sx, &sy);
            if (hit) return SurfaceHit{s->surface_id(hit), sx, sy};
        }
        return std::nullopt;
    });
}

// ---------------------------------------------------------------- capture / gamma

void ServerBackend::capture_done(uint64_t request_id, bool ok, NativeHandle render_done) {
    Server* s = impl_.get();
    int fd = fd_of(render_done);
    if (!s->dispatcher->post([s, request_id, ok, fd] { s->capture_done(request_id, ok, fd); }) && fd >= 0)
        ::close(fd);
}

std::optional<GammaChanged> ServerBackend::gamma(MonitorId output) const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.gamma.find(output);
    if (it == impl_->mirror.gamma.end()) return std::nullopt;
    return it->second;
}

// ---------------------------------------------------------------- virtual devices

void ServerBackend::inject_touch_down(int32_t id, double x, double y) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_touch(kInjectDown, id, x, y); });
}

void ServerBackend::inject_touch_motion(int32_t id, double x, double y) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_touch(kInjectMotion, id, x, y); });
}

void ServerBackend::inject_touch_up(int32_t id) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_touch(kInjectUp, id, 0, 0); });
}

void ServerBackend::inject_touch_frame() {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_touch(kInjectFrame, 0, 0, 0); });
}

void ServerBackend::inject_tablet_proximity(double x, double y, bool in) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_tablet(kInjectProximity, x, y, 0, 0, in); });
}

void ServerBackend::inject_tablet_motion(double x, double y, double pressure) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_tablet(kInjectToolMotion, x, y, pressure, 0, false); });
}

void ServerBackend::inject_tablet_tip(bool down) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_tablet(kInjectTip, 0, 0, 0, 0, down); });
}

void ServerBackend::inject_tablet_button(uint32_t button, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_tablet(kInjectToolButton, 0, 0, 0, button, pressed); });
}

void ServerBackend::inject_tablet_pad_button(uint32_t button, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_tablet(kInjectPadButton, 0, 0, 0, button, pressed); });
}

}  // namespace brocompositor::wl
