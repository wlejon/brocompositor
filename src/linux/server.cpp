// ServerBackend: the host-facing facade. Queries read the mirror; mutations
// run on the server thread through the dispatcher.
#include "brocompositor/linux/server.h"

#include "linux/client_surface.h"
#include "linux/server_impl.h"

#include <unistd.h>

#include <future>
#include <type_traits>
#include <variant>

namespace brocompositor::wl {

struct ServerBackend::Impl : Server {};

ServerBackend::ServerBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<ServerBackend> ServerBackend::create(const ServerConfig& config, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->dispatcher = std::make_shared<Dispatcher>();
    if (!impl->dispatcher->valid()) {
        if (error) *error = "eventfd failed";
        return nullptr;
    }
    Server* s = impl.get();
    std::promise<std::string> ready;
    auto started = ready.get_future();
    s->thread = std::thread([s, ready = std::move(ready)]() mutable {
        s->dispatcher->bind_thread();
        std::string err;
        if (!s->init(&err)) {
            s->shutdown();
            ready.set_value(err.empty() ? std::string("server init failed") : err);
            return;
        }
        s->running = true;
        ready.set_value(std::string());
        wl_display_run(s->display);
        s->running = false;
        s->dispatcher->close();
        s->shutdown();
    });
    std::string err = started.get();
    if (!err.empty()) {
        s->thread.join();
        if (error) *error = err;
        return nullptr;
    }
    return std::unique_ptr<ServerBackend>(new ServerBackend(std::move(impl)));
}

ServerBackend::~ServerBackend() {
    Server* s = impl_.get();
    s->dispatcher->post([s] { wl_display_terminate(s->display); });
    if (s->thread.joinable()) s->thread.join();
    s->dispatcher->close();
}

EventQueue& ServerBackend::events() { return impl_->events; }
ServerEventQueue& ServerBackend::server_events() { return impl_->server_events; }
std::string ServerBackend::socket_name() const { return impl_->socket; }
std::string ServerBackend::runtime_dir() const { return impl_->runtime_dir; }
AdapterId ServerBackend::adapter() const { return impl_->adapter; }

// ---------------------------------------------------------------- commands

bool ServerBackend::execute(const Command& command) {
    return std::visit(
        [&](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, PlaceWindow>) return place(c.id, c.frame);
            else if constexpr (std::is_same_v<T, SetWindowVisible>) return set_visible(c.id, c.visible);
            else if constexpr (std::is_same_v<T, FocusWindow>) return focus(c.id);
            else if constexpr (std::is_same_v<T, CloseWindow>) return close(c.id);
            else if constexpr (std::is_same_v<T, SetWindowState>) return apply_state(c.id, c.state);
            else static_assert(sizeof(T) == 0, "unhandled Command");
        },
        command);
}

// The xdg / X11 maximized and fullscreen flags, plus minimize. Minimized
// windows are hidden (xdg-shell has no client-side minimize, so the server
// simply stops drawing them); leaving it shows the window again.
bool ServerBackend::apply_state(WindowId id, WindowState state) {
    auto snap = query(id);
    if (!snap) return false;
    if (state == WindowState::Minimized) {
        bool ok = set_window_minimized(id, true);
        return set_visible(id, false) && ok;
    }
    bool ok = set_window_state(id, state == WindowState::Maximized, state == WindowState::Fullscreen);
    if (snap->minimized) {
        ok = set_window_minimized(id, false) && ok;
        ok = set_visible(id, true) && ok;
    }
    return ok;
}

size_t ServerBackend::execute(const std::vector<Command>& commands) {
    size_t failures = 0;
    for (const auto& c : commands) failures += execute(c) ? 0 : 1;
    return failures;
}

bool ServerBackend::place(WindowId id, const Rect& frame) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, id, frame] { return s->place_window(id, frame); });
}

bool ServerBackend::set_visible(WindowId id, bool visible) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, id, visible] { return s->set_window_visible(id, visible); });
}

bool ServerBackend::focus(WindowId id) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, id] {
        if (id != kNoWindow && !s->window_ref(id)) return false;
        s->focus_window(id);
        return true;
    });
}

bool ServerBackend::close(WindowId id) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, id] { return s->close_window(id); });
}

// ---------------------------------------------------------------- windows

std::optional<WindowSnapshot> ServerBackend::query(WindowId id) const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.windows.find(id);
    if (it == impl_->mirror.windows.end()) return std::nullopt;
    return it->second.snap;
}

std::vector<WindowId> ServerBackend::windows() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    std::vector<WindowId> out;
    for (auto& [id, w] : impl_->mirror.windows) out.push_back(id);
    return out;
}

bool ServerBackend::visible(WindowId id) const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.windows.find(id);
    return it != impl_->mirror.windows.end() && it->second.visible;
}

bool ServerBackend::server_side_decoration(WindowId id) const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.windows.find(id);
    return it != impl_->mirror.windows.end() && it->second.ssd;
}

bool ServerBackend::set_window_state(WindowId id, bool maximized, bool fullscreen) {
    Server* s = impl_.get();
    return s->dispatcher->call([=] { return s->set_window_state(id, maximized, fullscreen); });
}

// While the session is locked, client content is withheld from the host:
// trees are empty, hit tests miss and surface() only finds lock surfaces.
std::vector<SurfaceNode> ServerBackend::window_surfaces(WindowId id) const {
    if (impl_->lock_gate->locked) return {};
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.windows.find(id);
    if (it == impl_->mirror.windows.end()) return {};
    return it->second.tree;
}

std::optional<SurfaceHit> ServerBackend::hit_test(WindowId id, double wx, double wy) const {
    Server* s = impl_.get();
    return s->dispatcher->call([=]() -> std::optional<SurfaceHit> { return s->hit_test_window(id, wx, wy); });
}

// ---------------------------------------------------------------- layers

std::vector<LayerSurfaceInfo> ServerBackend::layer_surfaces() const {
    if (impl_->lock_gate->locked) return {};
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    std::vector<LayerSurfaceInfo> out;
    for (auto& [id, l] : impl_->mirror.layers)
        if (l.info.mapped) out.push_back(l.info);
    return out;
}

std::vector<SurfaceNode> ServerBackend::layer_surface_tree(LayerSurfaceId id) const {
    if (impl_->lock_gate->locked) return {};
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.layers.find(id);
    if (it == impl_->mirror.layers.end()) return {};
    return it->second.tree;
}

std::optional<SurfaceHit> ServerBackend::hit_test_layer(LayerSurfaceId id, double lx, double ly) const {
    Server* s = impl_.get();
    return s->dispatcher->call([=]() -> std::optional<SurfaceHit> {
        LayerRec* l = s->layer_by_id(id);
        if (!l || s->locked()) return std::nullopt;
        double sx = 0, sy = 0;
        wlr_surface* hit = wlr_layer_surface_v1_surface_at(l->layer, lx, ly, &sx, &sy);
        if (!hit) return std::nullopt;
        return SurfaceHit{s->surface_id(hit), sx, sy};
    });
}

bool ServerBackend::focus_layer_surface(LayerSurfaceId id) {
    Server* s = impl_.get();
    return s->dispatcher->call([=] {
        LayerRec* l = s->layer_by_id(id);
        if (!l || !l->mapped) return false;
        s->focus_surface(l->layer->surface);
        return true;
    });
}

// ---------------------------------------------------------------- surfaces

std::shared_ptr<ClientSurface> ServerBackend::surface(SurfaceId id) const {
    if (!impl_->lock_gate->allows(id)) return nullptr;
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.surfaces.find(id);
    if (it == impl_->mirror.surfaces.end()) return nullptr;
    return it->second;
}

CursorChanged ServerBackend::cursor() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.cursor;
}

std::optional<SurfaceNode> ServerBackend::drag_icon() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.drag_icon;
}

// ---------------------------------------------------------------- outputs

std::vector<MonitorSnapshot> ServerBackend::monitors() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return impl_->mirror.monitors;
}

std::vector<OutputInfo> ServerBackend::outputs() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    std::vector<OutputInfo> out;
    for (auto& [id, o] : impl_->mirror.outputs) out.push_back(o.info);
    return out;
}

std::vector<SharedImage> ServerBackend::output_images(MonitorId output) const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    std::vector<SharedImage> out;
    auto it = impl_->mirror.outputs.find(output);
    if (it != impl_->mirror.outputs.end())
        for (auto& img : it->second.images) out.push_back(img.desc);
    return out;
}

std::optional<SharedImage> ServerBackend::acquire_output_image(MonitorId output) {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    auto it = impl_->mirror.outputs.find(output);
    if (it == impl_->mirror.outputs.end()) return std::nullopt;
    for (auto& img : it->second.images)
        if (img.state == SlotState::Free) {
            img.state = SlotState::Host;
            return img.desc;
        }
    return std::nullopt;
}

bool ServerBackend::present_output(MonitorId output, PresentRequest request) {
    {
        std::lock_guard<std::mutex> lock(impl_->mirror.m);
        auto it = impl_->mirror.outputs.find(output);
        bool known = false;
        if (it != impl_->mirror.outputs.end())
            for (auto& img : it->second.images)
                if (img.desc.id == request.image_id) {
                    known = true;
                    img.state = SlotState::Queued;
                }
        if (!known) {
            if (int fd = fd_of(request.render_done); fd >= 0) ::close(fd);
            return false;
        }
    }
    Server* s = impl_.get();
    return s->dispatcher->post([s, output, req = std::move(request)]() mutable { s->present(output, std::move(req)); });
}

void ServerBackend::discard_output_image(MonitorId output, uint64_t image_id) {
    impl_->set_image_state(output, image_id, SlotState::Free);
}

void ServerBackend::schedule_frame(MonitorId output) {
    Server* s = impl_.get();
    s->dispatcher->post([s, output] {
        if (OutputRec* o = s->output_rec(output)) wlr_output_schedule_frame(o->output);
    });
}

bool ServerBackend::configure_output(MonitorId output, const OutputConfig& config) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, output, config] { return s->configure_output(output, config); });
}

MonitorId ServerBackend::add_output(Size size) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, size] {
        MonitorId id = s->add_output(size);
        s->publish_outputs();
        return id;
    });
}

ReservationId ServerBackend::reserve_edge(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted) {
    Server* s = impl_.get();
    auto r = s->dispatcher->call([=] {
        Rect g;
        ReservationId id = s->reserve_edge(monitor, edge, thickness, &g);
        return std::pair<ReservationId, Rect>{id, g};
    });
    if (granted) *granted = r.second;
    return r.first;
}

bool ServerBackend::release_edge(ReservationId id) {
    Server* s = impl_.get();
    return s->dispatcher->call([s, id] { return s->release_edge(id); });
}

// ---------------------------------------------------------------- input

void ServerBackend::pointer_route(SurfaceId surface, double sx, double sy, uint32_t time_msec) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->pointer_route(surface, sx, sy, time_msec); });
}

void ServerBackend::pointer_button(uint32_t time_msec, uint32_t button, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] {
        // A client's synthesized click from before the lock, routed after it.
        if (s->routed_client_input_refused(time_msec, button, pressed, true)) return;
        wlr_seat_pointer_notify_button(s->seat, time_msec, button,
                                       pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    });
}

void ServerBackend::pointer_axis(uint32_t time_msec, uint32_t orientation, double delta, int32_t discrete,
                                 uint32_t source) {
    Server* s = impl_.get();
    s->dispatcher->post([=] {
        wlr_seat_pointer_notify_axis(s->seat, time_msec, wl_pointer_axis(orientation), delta, discrete,
                                     wl_pointer_axis_source(source), WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    });
}

void ServerBackend::pointer_frame() {
    Server* s = impl_.get();
    s->dispatcher->post([s] { wlr_seat_pointer_notify_frame(s->seat); });
}

void ServerBackend::keyboard_key(uint32_t time_msec, uint32_t keycode, bool pressed,
                                 const KeyboardModifiers& modifiers_after) {
    Server* s = impl_.get();
    s->dispatcher->post([=] {
        // A client's synthesized key from before the lock, routed after it.
        if (s->routed_client_input_refused(time_msec, keycode, pressed, false)) return;
        // An input method's keyboard grab sees routed keys first (never while
        // locked, and never keys the input method itself typed).
        if (s->im_grab_key(time_msec, keycode, pressed)) {
            s->im_grab_modifiers(modifiers_after);
            return;
        }
        wlr_seat_keyboard_notify_key(s->seat, time_msec, keycode,
                                     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        s->send_modifiers(modifiers_after);
    });
}

void ServerBackend::keyboard_key(uint32_t time_msec, uint32_t keycode, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] {
        if (s->routed_client_input_refused(time_msec, keycode, pressed, false)) return;
        // The host keyboard's keys are interpreted with the virtual keyboard's
        // keymap: make it the seat's (a virtual-keyboard-v1 client may have
        // switched the seat to its own) so clients hold that keymap.
        KeyboardRec* host = nullptr;
        for (auto& k : s->keyboards)
            if (k->keyboard == s->vkeyboard) host = k.get();
        if (s->vkeyboard && wlr_seat_get_keyboard(s->seat) != s->vkeyboard)
            wlr_seat_set_keyboard(s->seat, s->vkeyboard);
        KeyboardModifiers after = s->sent_modifiers;
        if (host && host->shadow) {
            xkb_state_update_key(host->shadow, keycode + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
            after.depressed = xkb_state_serialize_mods(host->shadow, XKB_STATE_MODS_DEPRESSED);
            after.latched = xkb_state_serialize_mods(host->shadow, XKB_STATE_MODS_LATCHED);
            after.locked = xkb_state_serialize_mods(host->shadow, XKB_STATE_MODS_LOCKED);
            after.group = xkb_state_serialize_layout(host->shadow, XKB_STATE_LAYOUT_EFFECTIVE);
        }
        if (s->im_grab_key(time_msec, keycode, pressed)) {
            s->im_grab_modifiers(after);
            return;
        }
        wlr_seat_keyboard_notify_key(s->seat, time_msec, keycode,
                                     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        s->send_modifiers(after);
    });
}

void ServerBackend::keyboard_modifiers(const KeyboardModifiers& modifiers) {
    Server* s = impl_.get();
    s->dispatcher->post([=] {
        if (!s->im_grab_modifiers(modifiers)) s->send_modifiers(modifiers);
    });
}

void ServerBackend::warp_cursor(double x, double y) {
    Server* s = impl_.get();
    s->dispatcher->post([=] {
        s->warp(x, y);
    });
}

std::pair<double, double> ServerBackend::cursor_position() const {
    std::lock_guard<std::mutex> lock(impl_->mirror.m);
    return {impl_->mirror.cursor_x, impl_->mirror.cursor_y};
}

void ServerBackend::inject_key(uint32_t keycode, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_key(keycode, pressed); });
}

void ServerBackend::inject_pointer_motion(double dx, double dy) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_pointer_motion(dx, dy); });
}

void ServerBackend::inject_pointer_warp(double x, double y, double dx, double dy) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_pointer_warp(x, y, dx, dy); });
}

void ServerBackend::inject_pointer_button(uint32_t button, bool pressed) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_pointer_button(button, pressed); });
}

void ServerBackend::inject_pointer_axis(uint32_t orientation, double delta, int32_t discrete) {
    Server* s = impl_.get();
    s->dispatcher->post([=] { s->inject_pointer_axis(orientation, delta, discrete); });
}

}  // namespace brocompositor::wl
