#include "linux/wl_harness.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

using namespace brocompositor;
using namespace brocompositor::wl;

namespace bctest {

namespace {

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

std::string private_runtime_dir() {
    char tmpl[] = "/tmp/bc-wl-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return {};
    chmod(dir, 0700);
    // Removed when the test exits normally (sockets, lock files, test files).
    static std::string created;
    created = dir;
    std::atexit([] {
        std::error_code ec;
        if (!created.empty()) std::filesystem::remove_all(created, ec);
    });
    setenv("XDG_RUNTIME_DIR", dir, 1);
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("WAYLAND_SOCKET");
    unsetenv("DISPLAY");
    // A client killed mid-write must not take the test down.
    signal(SIGPIPE, SIG_IGN);
    return dir;
}

void Host::set_presenting(bool on) {
    presenting_ = on;
    if (on)
        for (auto& o : server_->outputs())
            if (o.enabled) server_->schedule_frame(o.id);
}

bool Host::start(HostOptions options, std::string* error) {
    options_ = std::move(options);
    presenting_ = options_.present;
    server_ = ServerBackend::create(options_.server, error);
    if (!server_) return false;
    wm_ = std::make_unique<WindowManager>();
    auto wake = [this] {
        std::lock_guard<std::mutex> lock(wake_m_);
        woken_ = true;
        wake_cv_.notify_all();
    };
    server_->events().set_wake(wake);
    server_->server_events().set_wake(wake);
    thread_ = std::thread([this] { run(); });
    return true;
}

Host::~Host() { stop(); }

void Host::stop() {
    if (thread_.joinable()) {
        stop_ = true;
        {
            std::lock_guard<std::mutex> lock(wake_m_);
            woken_ = true;
        }
        wake_cv_.notify_all();
        thread_.join();
    }
    server_.reset();
}

std::vector<std::string> Host::client_env() const {
    std::string x = server_->xwayland_display();
    return {"WAYLAND_DISPLAY=" + server_->socket_name(), "XDG_RUNTIME_DIR=" + server_->runtime_dir(),
            x.empty() ? std::string("DISPLAY") : "DISPLAY=" + x, "GDK_BACKEND=wayland", "QT_QPA_PLATFORM=wayland",
            "WAYLAND_DEBUG", "XAUTHORITY"};
}

void Host::run() {
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lock(wake_m_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return woken_; });
            woken_ = false;
        }
        for (auto& e : server_->events().drain()) handle(e);
        for (auto& e : server_->server_events().drain()) handle(e);
    }
}

void Host::handle(const Event& e) {
    std::vector<Command> cmds;
    {
        std::lock_guard<std::mutex> lock(m_);
        cmds = wm_->handle(e);
        if (options_.focus_new_windows)
            if (auto* a = std::get_if<WindowAdded>(&e)) {
                auto more = wm_->focus(a->window.id);
                cmds.insert(cmds.end(), more.begin(), more.end());
            }
        events_.push_back(e);
    }
    server_->execute(cmds);
    log_cv_.notify_all();
}

void Host::handle(const ServerEvent& e) {
    std::visit(overloaded{
                   [&](const OutputFrame& f) {
                       if (presenting_) render(f.output);
                   },
                   [&](const OutputsChanged& o) {
                       std::lock_guard<std::mutex> lock(m_);
                       outputs_ = o.outputs;
                   },
                   [&](const PointerMotion& m) {
                       if (options_.route_input) route_pointer(m.x, m.y, m.time_msec);
                   },
                   [&](const PointerButton& b) {
                       if (!options_.route_input) return;
                       server_->pointer_button(b.time_msec, b.button, b.pressed);
                   },
                   [&](const PointerAxis& a) {
                       if (options_.route_input)
                           server_->pointer_axis(a.time_msec, a.orientation, a.delta, a.delta_discrete, a.source);
                   },
                   [&](const PointerFrame&) {
                       if (options_.route_input) server_->pointer_frame();
                   },
                   [&](const KeyboardKey& k) {
                       if (options_.route_input)
                           server_->keyboard_key(k.time_msec, k.keycode, k.pressed, k.modifiers_after);
                   },
                   [&](const SurfaceCommitted&) {},
                   [&](const WindowRequest& r) {
                       if (options_.honor_requests) handle_request(r);
                   },
                   [&](const CaptureRequest& r) {
                       if (options_.answer_captures) answer_capture(r);
                   },
                   [&](const auto&) {
                       if (options_.route_input) handle_input(e);
                   },
               },
               e);
    {
        std::lock_guard<std::mutex> lock(m_);
        sevents_.push_back(e);
    }
    log_cv_.notify_all();
}

void Host::route_pointer(double x, double y, uint32_t time) {
    Pick p = pick(x, y);
    server_->pointer_route(p.surface, x - p.ox, y - p.oy, time);
    pointer_surface_ = p.surface;
}

void Host::blit_tree(CpuMapping& dst, const Rect& out_layout, float scale, Point origin,
                     const std::vector<SurfaceNode>& tree, std::vector<SurfaceId>& drawn) {
    for (const SurfaceNode& n : tree) {
        auto src = server_->surface(n.surface);
        if (!src) continue;
        auto frame = src->acquire();
        if (!frame) continue;
        drawn.push_back(n.surface);
        if (auto img = src->image(frame->image_id)) {
            if (auto map = CpuMapping::map(*img, false)) {
                int32_t ox = int32_t(std::lround((origin.x + n.offset.x - out_layout.x) * scale));
                int32_t oy = int32_t(std::lround((origin.y + n.offset.y - out_layout.y) * scale));
                int32_t w = std::min<int32_t>(int32_t(map->width()), frame->content.width);
                int32_t h = std::min<int32_t>(int32_t(map->height()), frame->content.height);
                for (int32_t y = 0; y < h; ++y) {
                    int32_t dy = oy + y;
                    if (dy < 0 || dy >= int32_t(dst.height())) continue;
                    for (int32_t x = 0; x < w; ++x) {
                        int32_t dx = ox + x;
                        if (dx < 0 || dx >= int32_t(dst.width())) continue;
                        uint32_t p = map->argb(uint32_t(x), uint32_t(y));
                        if ((p >> 24) == 0) continue;
                        dst.fill_rect(Rect{dx, dy, 1, 1}, p | 0xFF000000u);
                    }
                }
            }
        }
        src->release(*frame);
    }
}

std::vector<Rect> Host::diff_damage(MonitorId output, const CpuMapping& map) {
    uint32_t w = map.width(), h = map.height();
    std::vector<uint32_t>& prev = last_pixels_[output];
    bool fresh = prev.size() != size_t(w) * h;
    if (fresh) prev.assign(size_t(w) * h, 0);
    int32_t x0 = int32_t(w), y0 = int32_t(h), x1 = -1, y1 = -1;
    for (uint32_t y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<const uint32_t*>(map.data() + size_t(y) * map.stride());
        uint32_t* old = &prev[size_t(y) * w];
        for (uint32_t x = 0; x < w; ++x)
            if (row[x] != old[x]) {
                old[x] = row[x];
                x0 = std::min(x0, int32_t(x));
                x1 = std::max(x1, int32_t(x));
                y0 = std::min(y0, int32_t(y));
                y1 = std::max(y1, int32_t(y));
            }
    }
    if (fresh) return {};
    if (x1 < 0) return {Rect{}};
    return {Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1}};
}

void Host::render(MonitorId output) {
    auto img = server_->acquire_output_image(output);
    if (!img) return;
    PresentRequest req;
    req.image_id = img->id;
    OutputInfo info;
    {
        std::lock_guard<std::mutex> lock(m_);
        for (auto& o : outputs_)
            if (o.id == output) info = o;
    }
    if (info.layout.empty())
        for (auto& o : server_->outputs())
            if (o.id == output) info = o;
    std::vector<SurfaceId> drawn;
    bool locked = server_->session_lock_state() != LockState::Unlocked;
    if (options_.composite && locked && options_.lock_aware) {
        if (auto map = CpuMapping::map(*img, true)) {
            map->fill(options_.lock_background);
            blit_tree(*map, info.layout, info.scale, Point{info.layout.x, info.layout.y},
                      server_->lock_surface_tree(output), drawn);
        }
    } else if (options_.composite) {
        if (auto map = CpuMapping::map(*img, true)) {
            map->fill(options_.background);
            auto layers = server_->layer_surfaces();
            auto draw_layers = [&](bool above) {
                for (auto& l : layers) {
                    if (l.monitor != output) continue;
                    bool is_above = l.layer == Layer::Top || l.layer == Layer::Overlay;
                    if (is_above != above) continue;
                    blit_tree(*map, info.layout, info.scale, Point{l.rect.x, l.rect.y},
                              server_->layer_surface_tree(l.id), drawn);
                }
            };
            draw_layers(false);
            std::vector<WindowId> order;
            {
                std::lock_guard<std::mutex> lock(m_);
                WindowId f = wm_->focused();
                for (WindowId id : wm_->windows())
                    if (id != f) order.push_back(id);
                if (f) order.push_back(f);
            }
            for (WindowId id : order) {
                auto snap = server_->query(id);
                if (!snap || !server_->visible(id)) continue;
                blit_tree(*map, info.layout, info.scale, Point{snap->frame.x, snap->frame.y},
                          server_->window_surfaces(id), drawn);
            }
            draw_layers(true);
            // X11 override-redirect (menus, tooltips) above everything.
            for (const auto& u : server_->unmanaged_surfaces())
                blit_tree(*map, info.layout, info.scale, Point{u.rect.x, u.rect.y},
                          {SurfaceNode{u.surface, Point{0, 0}, Size{u.rect.width, u.rect.height}, false}}, drawn);
        }
    }
    if (options_.track_damage)
        if (auto map = CpuMapping::map(*img, false)) req.damage = diff_damage(output, *map);
    req.surfaces = drawn;
    if (options_.render_hook) options_.render_hook(output, *img, req);
    if (server_->present_output(output, std::move(req))) {
        std::lock_guard<std::mutex> lock(m_);
        ++presents_[output];
        last_image_[output] = *img;
        last_drawn_[output] = std::set<SurfaceId>(drawn.begin(), drawn.end());
    }
    log_cv_.notify_all();
}

bool Host::wait(const std::function<bool()>& pred, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (pred()) return true;
        std::unique_lock<std::mutex> lock(m_);
        if (log_cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds(20))) ==
                std::cv_status::timeout &&
            std::chrono::steady_clock::now() >= deadline) {
            lock.unlock();
            return pred();
        }
    }
}

std::vector<Event> Host::events() const {
    std::lock_guard<std::mutex> lock(m_);
    return events_;
}

std::vector<ServerEvent> Host::server_events() const {
    std::lock_guard<std::mutex> lock(m_);
    return sevents_;
}

WindowId Host::window_by_app_id(const std::string& app_id) const {
    std::lock_guard<std::mutex> lock(m_);
    WindowId found = kNoWindow;
    for (WindowId id : wm_->windows())
        if (auto v = wm_->window(id); v && v->snapshot.app_id == app_id) found = std::max(found, id);
    return found;
}

std::optional<WindowSnapshot> Host::wm_window(WindowId id) const {
    std::lock_guard<std::mutex> lock(m_);
    auto v = wm_->window(id);
    if (!v) return std::nullopt;
    return v->snapshot;
}

WindowId Host::wm_focused() const {
    std::lock_guard<std::mutex> lock(m_);
    return wm_->focused();
}

std::vector<MonitorSnapshot> Host::wm_monitors() const {
    std::lock_guard<std::mutex> lock(m_);
    return wm_->monitors();
}

void Host::wm_do(const std::function<std::vector<Command>(WindowManager&)>& action) {
    std::vector<Command> cmds;
    {
        std::lock_guard<std::mutex> lock(m_);
        cmds = action(*wm_);
    }
    server_->execute(cmds);
}

std::optional<uint32_t> Host::surface_pixel(SurfaceId surface, uint32_t x, uint32_t y) {
    auto src = server_->surface(surface);
    if (!src) return std::nullopt;
    auto f = src->acquire();
    if (!f) return std::nullopt;
    std::optional<uint32_t> out;
    if (auto img = src->image(f->image_id))
        if (auto map = CpuMapping::map(*img, false)) out = map->argb(x, y);
    src->release(*f);
    return out;
}

std::optional<uint32_t> Host::output_pixel(MonitorId output, uint32_t x, uint32_t y) {
    SharedImage img;
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = last_image_.find(output);
        if (it == last_image_.end()) return std::nullopt;
        img = it->second;
    }
    auto map = CpuMapping::map(img, false);
    if (!map) return std::nullopt;
    return map->argb(x, y);
}

uint64_t Host::presents(MonitorId output) const {
    std::lock_guard<std::mutex> lock(m_);
    auto it = presents_.find(output);
    return it == presents_.end() ? 0 : it->second;
}

std::set<SurfaceId> Host::last_drawn(MonitorId output) const {
    std::lock_guard<std::mutex> lock(m_);
    auto it = last_drawn_.find(output);
    return it == last_drawn_.end() ? std::set<SurfaceId>{} : it->second;
}

}  // namespace bctest
