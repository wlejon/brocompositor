// Host behaviour for the desktop-session features: picking (lock surfaces,
// X11 unmanaged surfaces, windows), touch and tablet routing, window
// requests from clients and taskbars, and CPU answers to capture requests.
#include "linux/wl_harness.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

bool contains(const Rect& r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

}  // namespace

void tap_key(ServerBackend& server, uint32_t key) {
    server.inject_key(key, true);
    server.inject_key(key, false);
}

void type_text(ServerBackend& server, const std::string& text) {
    static const std::map<char, uint32_t> keys = {
        {'a', 30}, {'b', 48}, {'c', 46}, {'d', 32}, {'e', 18}, {'f', 33}, {'g', 34}, {'h', 35}, {'i', 23},
        {'j', 36}, {'k', 37}, {'l', 38}, {'m', 50}, {'n', 49}, {'o', 24}, {'p', 25}, {'q', 16}, {'r', 19},
        {'s', 31}, {'t', 20}, {'u', 22}, {'v', 47}, {'w', 17}, {'x', 45}, {'y', 21}, {'z', 44}, {'0', 11},
        {'1', 2},  {'2', 3},  {'3', 4},  {'4', 5},  {'5', 6},  {'6', 7},  {'7', 8},  {'8', 9},  {'9', 10},
        {'/', 53}, {'-', 12}, {'.', 52}, {' ', 57}, {'\n', 28}};
    constexpr uint32_t kShift = 42;
    for (char c : text) {
        bool upper = c >= 'A' && c <= 'Z';
        if (c == '_' || c == '>' || upper) {
            server.inject_key(kShift, true);
            tap_key(server, c == '_' ? 12u : c == '>' ? 52u : keys.at(char(c - 'A' + 'a')));
            server.inject_key(kShift, false);
            continue;
        }
        auto it = keys.find(c);
        if (it != keys.end()) tap_key(server, it->second);
    }
}

Host::Pick Host::pick(double x, double y) {
    std::vector<OutputInfo> outs;
    {
        std::lock_guard<std::mutex> lock(m_);
        outs = outputs_;
    }
    if (outs.empty()) outs = server_->outputs();
    if (server_->session_lock_state() != LockState::Unlocked) {
        for (const auto& o : outs)
            if (contains(o.layout, x, y))
                if (auto hit = server_->hit_test_lock(o.id, x - o.layout.x, y - o.layout.y))
                    return Pick{hit->surface, x - hit->sx, y - hit->sy};
        return {};
    }
    auto unmanaged = server_->unmanaged_surfaces();
    for (auto it = unmanaged.rbegin(); it != unmanaged.rend(); ++it)
        if (contains(it->rect, x, y)) return Pick{it->surface, double(it->rect.x), double(it->rect.y)};
    // Topmost window under the point: the focused one first, then the rest.
    std::vector<WindowId> order;
    {
        std::lock_guard<std::mutex> lock(m_);
        WindowId f = wm_->focused();
        if (f) order.push_back(f);
        for (WindowId id : wm_->windows())
            if (id != f) order.push_back(id);
    }
    for (WindowId id : order) {
        auto snap = server_->query(id);
        if (!snap || !server_->visible(id)) continue;
        if (auto hit = server_->hit_test(id, x - snap->frame.x, y - snap->frame.y))
            return Pick{hit->surface, x - hit->sx, y - hit->sy};
    }
    return {};
}

void Host::handle_input(const ServerEvent& e) {
    std::visit(overloaded{
                   [&](const TouchDown& t) {
                       Pick p = pick(t.x, t.y);
                       if (!p.surface) return;
                       touch_points_[t.id] = p;
                       server_->touch_down(p.surface, t.id, t.x - p.ox, t.y - p.oy, t.time_msec);
                   },
                   [&](const TouchMotion& t) {
                       auto it = touch_points_.find(t.id);
                       if (it == touch_points_.end()) return;
                       server_->touch_motion(t.id, t.x - it->second.ox, t.y - it->second.oy, t.time_msec);
                   },
                   [&](const TouchUp& t) {
                       if (!touch_points_.erase(t.id)) return;
                       server_->touch_up(t.id, t.time_msec);
                   },
                   [&](const TouchCancel&) {
                       touch_points_.clear();
                       server_->touch_cancel();
                   },
                   [&](const TouchFrame&) { server_->touch_frame(); },
                   [&](const TabletToolProximity& t) {
                       Pick p = t.in ? pick(t.x, t.y) : Pick{};
                       tool_focus_[t.tool] = p;
                       server_->tablet_tool_route(t.tool, p.surface, t.x - p.ox, t.y - p.oy, TabletToolAxes{});
                   },
                   [&](const TabletToolMotion& t) {
                       Pick p = pick(t.x, t.y);
                       tool_focus_[t.tool] = p;
                       server_->tablet_tool_route(t.tool, p.surface, t.x - p.ox, t.y - p.oy, t.axes);
                   },
                   [&](const TabletToolTip& t) { server_->tablet_tool_tip(t.tool, t.down); },
                   [&](const TabletToolButton& t) { server_->tablet_tool_button(t.tool, t.button, t.pressed); },
                   [&](const TabletPadButton& t) {
                       server_->tablet_pad_button(t.pad, t.time_msec, t.button, t.pressed);
                   },
                   [&](const TabletPadRing& t) {
                       server_->tablet_pad_ring(t.pad, t.time_msec, t.ring, t.position, t.finger);
                   },
                   [&](const TabletPadStrip& t) {
                       server_->tablet_pad_strip(t.pad, t.time_msec, t.strip, t.position, t.finger);
                   },
                   [&](const auto&) {},
               },
               e);
}

void Host::handle_request(const WindowRequest& r) {
    auto snap = server_->query(r.window);
    if (!snap) return;
    switch (r.kind) {
        case WindowRequestKind::Activate:
            if (snap->minimized) {
                server_->set_visible(r.window, true);
                server_->set_window_minimized(r.window, false);
            }
            wm_do([&](WindowManager& wm) { return wm.focus(r.window); });
            break;
        case WindowRequestKind::Close: server_->close(r.window); break;
        case WindowRequestKind::Minimize:
            server_->set_visible(r.window, false);
            server_->set_window_minimized(r.window, true);
            break;
        case WindowRequestKind::Unminimize:
            server_->set_visible(r.window, true);
            server_->set_window_minimized(r.window, false);
            break;
        case WindowRequestKind::Maximize: server_->set_window_state(r.window, true, snap->fullscreen); break;
        case WindowRequestKind::Unmaximize: server_->set_window_state(r.window, false, snap->fullscreen); break;
        case WindowRequestKind::Fullscreen: server_->set_window_state(r.window, snap->maximized, true); break;
        case WindowRequestKind::Unfullscreen: server_->set_window_state(r.window, snap->maximized, false); break;
        default: break;
    }
}

// The copy a GPU host would do, on the CPU: the output's presented image (or
// the window's surfaces) into the client's buffer.
void Host::answer_capture(const CaptureRequest& r) {
    bool ok = false;
    if (auto dst = CpuMapping::map(r.target, true)) {
        constexpr uint32_t kXBGR8888 = 0x34324258, kABGR8888 = 0x34324241;  // 'XB24', 'AB24'
        bool bgr = r.target.drm_format == kXBGR8888 || r.target.drm_format == kABGR8888;
        if (r.window == kNoWindow) {
            for (const SharedImage& img : server_->output_images(r.output)) {
                if (img.id != r.source_image) continue;
                auto src = CpuMapping::map(img, false);
                if (!src) break;
                uint32_t w = std::min<uint32_t>(dst->width(), uint32_t(std::max(0, r.region.width)));
                uint32_t h = std::min<uint32_t>(dst->height(), uint32_t(std::max(0, r.region.height)));
                for (uint32_t y = 0; y < h; ++y) {
                    auto* row = reinterpret_cast<uint32_t*>(dst->data() + size_t(y) * dst->stride());
                    for (uint32_t x = 0; x < w; ++x) {
                        uint32_t p = src->argb(uint32_t(r.region.x) + x, uint32_t(r.region.y) + y);
                        if (bgr) p = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
                        row[x] = p;
                    }
                }
                ok = true;
            }
        } else if (auto snap = server_->query(r.window)) {
            dst->fill(0);
            std::vector<SurfaceId> drawn;
            blit_tree(*dst, Rect{snap->frame.x, snap->frame.y, snap->frame.width, snap->frame.height}, r.scale,
                      Point{snap->frame.x, snap->frame.y}, server_->window_surfaces(r.window), drawn);
            ok = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        ++captures_answered_;
    }
    server_->capture_done(r.id, ok);
}

}  // namespace bctest
