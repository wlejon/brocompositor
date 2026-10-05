// The nested (Wayland) backend for development: a child ServerBackend whose
// outputs are windows of a parent ServerBackend (headless), each with its
// own Host. The child's output is a window in the parent, the child's CPU
// composite (background + its client) reaches the parent as that window's
// content, parent input on that window reaches the child's client through
// the child's own routing, resizing the window re-modes the child's output,
// and closing it removes the child's monitor.
#include "linux/wl_harness.h"
#include "printers.h"

#include <cstdlib>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

void run(Host& parent, Host& child) {
    REQUIRE(parent.wait([&] { return parent.wm_monitors().size() == 1; }));
    REQUIRE(child.wait([&] { return child.wm_monitors().size() == 1; }));
    MonitorId child_mon = child.wm_monitors()[0].id;
    CHECK_EQ(child.wm_monitors()[0].bounds, (Rect{0, 0, 400, 300}));

    // The child's output is a parent window showing the child's background.
    REQUIRE(parent.wait([&] { return parent.server().windows().size() == 1; }));
    WindowId nest = parent.server().windows()[0];
    auto snap = parent.server().query(nest);
    REQUIRE(snap);
    std::printf("nested output window: '%s' app_id '%s' %dx%d\n", snap->title.c_str(), snap->app_id.c_str(),
                snap->frame.width, snap->frame.height);
    CHECK_EQ((Size{snap->frame.width, snap->frame.height}), (Size{400, 300}));
    SurfaceId nest_root = SurfaceId(snap->native);
    CHECK(parent.wait([&] { return parent.surface_pixel(nest_root, 5, 5) == 0xFF102030u; }));

    // A client of the child shows up inside it, through two compositors.
    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "inner", "--size", "120x80", "--color", "FFFF8800"},
                          child.client_env());
    REQUIRE(c);
    REQUIRE(c->wait_line("ready", 5000));
    REQUIRE(child.wait([&] { return child.window_by_app_id("inner") != kNoWindow; }));
    WindowId inner = child.window_by_app_id("inner");
    Rect f = child.server().query(inner)->frame;
    CHECK_EQ(f, (Rect{(400 - 120) / 2, (300 - 80) / 2, 120, 80}));
    CHECK(parent.wait([&] {
        return parent.surface_pixel(nest_root, uint32_t(f.x + 60), uint32_t(f.y + 40)) == 0xFFFF8800u;
    }));
    CHECK(parent.wait([&] {
        Rect pf = parent.server().query(nest)->frame;
        return parent.output_pixel(parent.wm_monitors()[0].id, uint32_t(pf.x + f.x + 60),
                                   uint32_t(pf.y + f.y + 40)) == 0xFFFF8800u;
    }));

    // Parent input -> nested output window -> child seat -> child routing -> client.
    snap = parent.server().query(nest);
    // wlroots' Wayland backend reports pointer position only on motion (not on
    // wl_pointer.enter), so move a pixel after entering, as a real mouse would.
    parent.server().inject_pointer_warp(snap->frame.x + f.x + 10, snap->frame.y + f.y + 20);
    parent.server().inject_pointer_motion(1, 1);
    CHECK(c->wait_line("enter 11 21", 5000));
    parent.server().inject_pointer_button(272, true);
    parent.server().inject_pointer_button(272, false);
    CHECK(c->wait_line("button 272 1", 5000));
    CHECK(parent.wait([&] { return parent.wm_focused() == nest; }));
    CHECK(child.wait([&] { return child.wm_focused() == inner; }));
    parent.server().inject_key(30, true);
    parent.server().inject_key(30, false);
    CHECK(c->wait_line("key 30 1", 5000));

    // Resizing the window re-modes the child's output.
    CHECK(parent.server().execute(Command{PlaceWindow{nest, Rect{10, 10, 600, 400}}}));
    CHECK(child.wait([&] {
        auto m = child.wm_monitors();
        return m.size() == 1 && m[0].bounds == Rect{0, 0, 600, 400};
    }));
    CHECK(child.wait([&] {
        for (auto& o : child.server().outputs())
            if (o.id == child_mon) return o.pixel_size == Size{600, 400};
        return false;
    }));
    CHECK(parent.wait([&] {
        auto s = parent.server().query(nest);
        return s && s->frame.width == 600 && s->frame.height == 400;
    }));

    // Closing the window removes the child's monitor.
    CHECK(parent.server().execute(Command{CloseWindow{nest}}));
    CHECK(child.wait([&] { return child.wm_monitors().empty(); }));
    CHECK(parent.wait([&] { return parent.server().windows().empty(); }));
    c->kill_now();
}

// X11 nested backend under Xvfb: the output comes up, presents complete
// through the X server (MIT-SHM buffers), and a client maps.
void run_x11() {
    if (which("Xvfb").empty()) {
        std::printf("SKIP x11: Xvfb not installed\n");
        return;
    }
    // -displayfd: Xvfb picks a free display and prints its number once it
    // accepts connections (no fixed-number collisions, no readiness race).
    auto x = Child::spawn({"Xvfb", "-displayfd", "1", "-screen", "0", "1024x768x24", "-nolisten", "tcp"}, {});
    REQUIRE(x);
    std::string number;
    REQUIRE(x->wait_line("", 10000, &number));
    while (!number.empty() && (number.back() == '\n' || number.back() == '\r' || number.back() == ' ')) number.pop_back();
    REQUIRE(!number.empty());
    const std::string display = ":" + number;
    setenv("DISPLAY", display.c_str(), 1);
    Host h;
    HostOptions o;
    o.server.backend = BackendKind::X11;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{320, 240};
    std::string err;
    bool started = h.start(o, &err);
    unsetenv("DISPLAY");
    if (!started) std::fprintf(stderr, "x11 child: %s\n", err.c_str());
    REQUIRE(started);
    CHECK(h.wait([&] { return h.wm_monitors().size() == 1; }));
    if (h.wm_monitors().size() == 1) {
        MonitorId m = h.wm_monitors()[0].id;
        CHECK_EQ(h.wm_monitors()[0].bounds, (Rect{0, 0, 320, 240}));
        CHECK(h.wait([&] {
            size_t n = 0;
            for (auto& p : h.server_events_of<OutputPresented>())
                if (p.output == m && p.presented) ++n;
            return n >= 10;
        }));
        CHECK(h.server_events_of<OutputPresentFailed>().empty());
        auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "x11-inner", "--size", "50x50"}, h.client_env());
        REQUIRE(c);
        CHECK(c->wait_line("frame 1", 5000));
        CHECK(h.wait([&] { return h.window_by_app_id("x11-inner") != kNoWindow; }));
        c->kill_now();
    }
    h.stop();
    x->terminate();
    std::printf("x11 nested: ok\n");
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host parent;
    HostOptions po;
    po.server.backend = BackendKind::Headless;
    po.server.output_buffers = OutputBufferKind::Shm;
    po.server.initial_output_size = Size{1024, 768};
    std::string err;
    if (!parent.start(po, &err)) {
        std::fprintf(stderr, "parent: %s\n", err.c_str());
        return 1;
    }
    // The child's Wayland backend connects to WAYLAND_DISPLAY.
    setenv("WAYLAND_DISPLAY", parent.server().socket_name().c_str(), 1);
    Host child;
    HostOptions co;
    co.server.backend = BackendKind::Wayland;
    co.server.output_buffers = OutputBufferKind::Shm;
    co.server.initial_output_size = Size{400, 300};
    co.background = 0xFF102030;
    if (!child.start(co, &err)) {
        std::fprintf(stderr, "child: %s\n", err.c_str());
        return 1;
    }
    unsetenv("WAYLAND_DISPLAY");
    run(parent, child);
    child.stop();
    parent.stop();
    run_x11();
    return finish("test_wl_nested");
}
