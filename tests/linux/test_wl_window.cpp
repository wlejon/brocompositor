// Window lifecycle through the server role, driven by a real Wayland client
// (bc_wl_client) on the headless backend: map -> WindowAdded with app_id and
// title, buffer content reaches the host as a SurfaceSource, subsurface and
// popup form the window's surface tree, the host's CPU composite shows all
// three, PlaceWindow -> configure -> WindowChanged, the WM's tiling layout
// round-trips, workspace hiding stops drawing, CloseWindow closes the client.
#include "linux/wl_harness.h"
#include "printers.h"

#include <cstdio>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

std::vector<std::string> client_args(const std::string& app_id, const std::string& color,
                                     std::vector<std::string> extra = {}) {
    std::vector<std::string> a = {BC_WL_CLIENT, "--app-id", app_id, "--title", "Title " + app_id, "--size", "200x150",
                                  "--color", color};
    a.insert(a.end(), extra.begin(), extra.end());
    return a;
}

void run(Host& host) {
    const Rect screen{0, 0, 1024, 768};
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    CHECK_EQ(host.wm_monitors()[0].bounds, screen);
    CHECK_EQ(host.wm_monitors()[0].work_area, screen);

    auto a = Child::spawn(client_args("bc-a", "FFFF0000", {"--subsurface", "--popup"}), host.client_env());
    REQUIRE(a);
    REQUIRE(host.wait([&] { return host.window_by_app_id("bc-a") != kNoWindow; }));
    WindowId wa = host.window_by_app_id("bc-a");
    auto snap = host.server().query(wa);
    REQUIRE(snap);
    CHECK_EQ(snap->title, std::string("Title bc-a"));
    CHECK_EQ(snap->class_name, std::string("xdg_toplevel"));
    CHECK(snap->process_id == uint32_t(a->pid()));
    CHECK_EQ(snap->frame, (Rect{(1024 - 200) / 2, (768 - 150) / 2, 200, 150}));
    CHECK_EQ(snap->monitor, host.wm_monitors()[0].id);

    // Content: the root surface's newest frame is the client's red buffer.
    REQUIRE(a->wait_line("ready", 5000));
    // xdg-decoration: no preference from the client -> server-side (host draws).
    CHECK(a->wait_line("decoration server", 5000));
    CHECK(host.server().server_side_decoration(wa));
    SurfaceId root = SurfaceId(snap->native);
    CHECK(host.wait([&] { return host.surface_pixel(root, 100, 75) == 0xFFFF0000u; }));
    auto src = host.server().surface(root);
    REQUIRE(src);
    CHECK_EQ(src->state().size, (Size{200, 150}));
    CHECK(src->images().size() >= 1);
    CHECK(src->images()[0].type == ImageHandleType::ShmFd);

    // Tree: root, subsurface (10,10 20x20), popup (anchored at 50,60, 40x30).
    REQUIRE(a->wait_line("popup_mapped", 5000));
    CHECK(host.wait([&] { return host.server().window_surfaces(wa).size() == 3; }));
    auto tree = host.server().window_surfaces(wa);
    if (tree.size() == 3) {
        CHECK_EQ(tree[0].offset, (Point{0, 0}));
        CHECK_EQ(tree[0].size, (Size{200, 150}));
        CHECK_EQ(tree[1].offset, (Point{10, 10}));
        CHECK_EQ(tree[1].size, (Size{20, 20}));
        CHECK(!tree[1].popup);
        CHECK_EQ(tree[2].offset, (Point{50, 60}));
        CHECK_EQ(tree[2].size, (Size{40, 30}));
        CHECK(tree[2].popup);
    }
    // The host's composite shows all three, and the frame callbacks flow.
    MonitorId mon = host.wm_monitors()[0].id;
    Point o{snap->frame.x, snap->frame.y};
    CHECK(host.wait([&] {
        return host.output_pixel(mon, uint32_t(o.x + 150), uint32_t(o.y + 20)) == 0xFFFF0000u &&
               host.output_pixel(mon, uint32_t(o.x + 15), uint32_t(o.y + 15)) == 0xFF00FF00u &&
               host.output_pixel(mon, uint32_t(o.x + 60), uint32_t(o.y + 70)) == 0xFF0000FFu;
    }));
    CHECK(a->wait_line("frame", 5000));

    // Focus: new windows are focused (FocusWindow -> keyboard enter).
    CHECK(host.wait([&] { return host.wm_focused() == wa; }));
    CHECK(a->wait_line("kbenter", 5000));

    // PlaceWindow -> configure -> the client resizes -> WindowChanged.
    REQUIRE(host.server().place(wa, Rect{100, 120, 320, 240}));
    CHECK(a->wait_line("configure 320 240", 5000));
    CHECK(host.wait([&] {
        auto w = host.wm_window(wa);
        return w && w->frame == Rect{100, 120, 320, 240};
    }));
    CHECK(host.wait([&] { return src->state().size == Size{320, 240}; }));

    // A second window, then tiling: the WM's layout reaches both clients.
    auto b = Child::spawn(client_args("bc-b", "FF00FFFF"), host.client_env());
    REQUIRE(b);
    REQUIRE(host.wait([&] { return host.window_by_app_id("bc-b") != kNoWindow; }));
    WindowId wb = host.window_by_app_id("bc-b");
    CHECK(host.wait([&] { return host.wm_focused() == wb; }));
    CHECK(a->wait_line("kbleave", 5000));
    WorkspaceId ws = kNoWorkspace;
    host.wm_do([&](WindowManager& wm) {
        ws = wm.active_workspace(mon);
        return wm.set_layout(ws, LayoutMode::Columns);
    });
    auto expect = compute_layout(LayoutMode::Columns, screen, {wa, wb}, LayoutConfig{});
    CHECK(host.wait([&] {
        auto fa = host.wm_window(wa), fb = host.wm_window(wb);
        return fa && fb && fa->frame == *expect.find(wa) && fb->frame == *expect.find(wb);
    }));
    char want[64];
    std::snprintf(want, sizeof want, "configure %d %d", expect.find(wb)->width, expect.find(wb)->height);
    CHECK(b->wait_line(want, 5000));

    // Workspace switch: both windows hidden, not drawn, then back.
    WorkspaceId ws2 = kNoWorkspace;
    host.wm_do([&](WindowManager& wm) {
        ws2 = wm.add_workspace(mon, "two");
        return wm.activate_workspace(ws2);
    });
    CHECK(host.wait([&] { return !host.server().visible(wa) && !host.server().visible(wb); }));
    CHECK(host.wait([&] {
        auto d = host.last_drawn(mon);
        return !d.count(root) && host.output_pixel(mon, uint32_t(expect.find(wa)->x + 5),
                                                   uint32_t(expect.find(wa)->y + 5)) == 0xFF203040u;
    }));
    host.wm_do([&](WindowManager& wm) { return wm.activate_workspace(ws); });
    CHECK(host.wait([&] { return host.server().visible(wa) && host.server().visible(wb); }));
    CHECK(host.wait([&] { return host.last_drawn(mon).count(root) == 1; }));

    // Window states through the WM: a shell reservation, maximize into the
    // work area it leaves, minimize hides, restore brings it back maximized.
    host.wm_do([&](WindowManager& wm) { return wm.set_layout(ws, LayoutMode::Floating); });
    ReservationId bar = kNoReservation;
    host.wm_do([&](WindowManager& wm) {
        auto r = wm.reserve_edge(mon, Edge::Top, 30);
        bar = r.id;
        return r.commands;
    });
    const Rect work{screen.x, screen.y + 30, screen.width, screen.height - 30};
    std::optional<Rect> before;
    host.wm_do([&](WindowManager& wm) {
        before = wm.window(wb)->snapshot.frame;
        return wm.maximize(wb);
    });
    CHECK(host.wait([&] {
        auto w = host.wm_window(wb);
        return w && w->maximized && w->frame == work;
    }));
    host.wm_do([&](WindowManager& wm) { return wm.minimize(wb); });
    CHECK(host.wait([&] {
        auto w = host.wm_window(wb);
        return w && w->minimized && !host.server().visible(wb);
    }));
    host.wm_do([&](WindowManager& wm) { return wm.restore(wb); });
    CHECK(host.wait([&] {
        auto w = host.wm_window(wb);
        return w && !w->minimized && w->maximized && host.server().visible(wb);
    }));
    host.wm_do([&](WindowManager& wm) { return wm.restore(wb); });
    CHECK(host.wait([&] {
        auto w = host.wm_window(wb);
        return w && !w->maximized && before && w->frame == *before;
    }));
    host.wm_do([&](WindowManager& wm) { return wm.release_edge(bar); });

    // CloseWindow -> xdg_toplevel.close -> the client exits -> WindowRemoved.
    CHECK(host.server().execute(Command{CloseWindow{wb}}));
    CHECK(b->wait_line("close", 5000));
    int status = -1;
    CHECK(b->wait_exit(5000, &status));
    CHECK_EQ(status, 0);
    CHECK(host.wait([&] {
        for (auto& r : host.events_of<WindowRemoved>())
            if (r.id == wb) return true;
        return false;
    }));
    // The core refocuses the most recent window.
    CHECK(host.wait([&] { return host.wm_focused() == wa; }));

    // A client that dies: its window goes away and its surfaces close.
    a->kill_now();
    CHECK(host.wait([&] { return host.window_by_app_id("bc-a") == kNoWindow; }));
    CHECK(host.wait([&] { return src->closed(); }));
    CHECK(host.server().windows().empty());

    // wp_viewporter: a 200x100 buffer cropped to (50,25 100x50) and shown at
    // 100x50 -- the logical size and the crop reach the host.
    // (This client also asks for client-side decorations.)
    auto v = Child::spawn({BC_WL_CLIENT, "--app-id", "bc-v", "--size", "200x100", "--viewport", "100x50", "--crop",
                           "50,25,100,50", "--csd"},
                          host.client_env());
    REQUIRE(v);
    REQUIRE(v->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("bc-v") != kNoWindow; }));
    auto vs = host.server().query(host.window_by_app_id("bc-v"));
    REQUIRE(vs);
    CHECK_EQ((Size{vs->frame.width, vs->frame.height}), (Size{100, 50}));
    auto vsrc = host.server().surface(SurfaceId(vs->native));
    REQUIRE(vsrc);
    CHECK(host.wait([&] { return vsrc->state().buffer_size == Size{200, 100}; }));
    SurfaceState st = vsrc->state();
    CHECK_EQ(st.size, (Size{100, 50}));
    CHECK(st.has_source_crop);
    CHECK(st.src_x == 50 && st.src_y == 25 && st.src_width == 100 && st.src_height == 50);
    CHECK(v->wait_line("decoration client", 5000));
    CHECK(!host.server().server_side_decoration(host.window_by_app_id("bc-v")));
    auto vt = host.server().window_surfaces(host.window_by_app_id("bc-v"));
    CHECK(vt.size() == 1 && vt[0].size == (Size{100, 50}));
    v->kill_now();
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) {
        std::fprintf(stderr, "cannot create a runtime dir\n");
        return 1;
    }
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{1024, 768};
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    run(host);
    host.stop();
    return finish("test_wl_window");
}
