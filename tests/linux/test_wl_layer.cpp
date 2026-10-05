// Layer shell through the server role: a top panel with an exclusive zone
// is configured to the output width, becomes a ReservationChanged and
// shrinks the monitor work area (MonitorsChanged into the WindowManager),
// draws above windows in the host composite, stacks with the host's own
// reserve_edge, and gives the zone back when it goes away. An overlay layer
// surface with exclusive keyboard interactivity takes keyboard focus from
// the focused window and returns it on unmap.
#include "linux/wl_harness.h"
#include "printers.h"

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

constexpr uint32_t kTop = 1, kBottom = 2, kLeft = 4, kRight = 8;

Rect work_area(Host& host) {
    auto m = host.wm_monitors();
    return m.empty() ? Rect{} : m[0].work_area;
}

bool has_reservation(Host& host, const Rect& rect) {
    for (auto& r : host.events_of<ReservationChanged>())
        if (r.rect == rect) return true;
    return false;
}

void run(Host& host) {
    const Rect screen{0, 0, 1024, 768};
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    MonitorId mon = host.wm_monitors()[0].id;

    // A top panel: anchored top+left+right, 30 px tall, exclusive 30.
    auto panel = Child::spawn({BC_WL_CLIENT, "--layer", "top", "--anchor", std::to_string(kTop | kLeft | kRight),
                               "--size", "0x30", "--exclusive", "30", "--color", "FF0000FF"},
                              host.client_env());
    REQUIRE(panel);
    CHECK(panel->wait_line("configure 1024 30", 5000));
    CHECK(panel->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return !host.server_events_of<LayerSurfaceAdded>().empty(); }));
    LayerSurfaceInfo info = host.server_events_of<LayerSurfaceAdded>()[0].info;
    CHECK_EQ(info.rect, (Rect{0, 0, 1024, 30}));
    CHECK(info.layer == Layer::Top);
    CHECK_EQ(info.name_space, std::string("bc-test"));
    CHECK_EQ(info.monitor, mon);
    CHECK(info.process_id == uint32_t(panel->pid()));
    CHECK(info.reservation != kNoReservation);
    CHECK(host.wait([&] { return has_reservation(host, Rect{0, 0, 1024, 30}); }));
    CHECK(host.wait([&] { return work_area(host) == Rect{0, 30, 1024, 738}; }));
    CHECK_EQ(host.server().monitors()[0].work_area, (Rect{0, 30, 1024, 738}));
    CHECK_EQ(host.server().monitors()[0].bounds, screen);

    // The panel is drawn into the output.
    CHECK(host.wait([&] { return host.output_pixel(mon, 500, 10) == 0xFF0000FFu; }));
    auto tree = host.server().layer_surface_tree(info.id);
    CHECK_EQ(tree.size(), size_t(1));
    auto hit = host.server().hit_test_layer(info.id, 20, 10);
    CHECK(hit && hit->surface == tree[0].surface);

    // A window maps inside the work area, below the panel.
    auto w = Child::spawn({BC_WL_CLIENT, "--app-id", "lw", "--size", "300x200"}, host.client_env());
    REQUIRE(w);
    REQUIRE(host.wait([&] { return host.window_by_app_id("lw") != kNoWindow; }));
    WindowId wid = host.window_by_app_id("lw");
    CHECK(w->wait_line("kbenter", 5000));
    auto snap = host.server().query(wid);
    REQUIRE(snap);
    CHECK(snap->frame.y >= 30);
    CHECK_EQ(snap->frame, (Rect{(1024 - 300) / 2, 30 + (738 - 200) / 2, 300, 200}));

    // The host reserves the bottom edge too; both shrink the work area.
    Rect granted;
    ReservationId host_res = host.server().reserve_edge(mon, Edge::Bottom, 40, &granted);
    CHECK(host_res != kNoReservation);
    CHECK_EQ(granted, (Rect{0, 728, 1024, 40}));
    CHECK(host.wait([&] { return work_area(host) == Rect{0, 30, 1024, 698}; }));
    CHECK(host.server().release_edge(host_res));
    CHECK(host.wait([&] { return work_area(host) == Rect{0, 30, 1024, 738}; }));
    CHECK(!host.server().release_edge(host_res));

    // An overlay with exclusive keyboard interactivity takes focus.
    auto overlay = Child::spawn({BC_WL_CLIENT, "--layer", "overlay", "--anchor", std::to_string(kBottom),
                                 "--size", "200x50", "--keyboard", "1", "--color", "FFFFFF00"},
                                host.client_env());
    REQUIRE(overlay);
    CHECK(overlay->wait_line("configure 200 50", 5000));
    CHECK(overlay->wait_line("kbenter", 5000));
    CHECK(w->wait_line("kbleave", 5000));
    host.server().inject_key(30, true);
    host.server().inject_key(30, false);
    CHECK(overlay->wait_line("key 30 1", 5000));
    CHECK_EQ(w->count("key 30"), size_t(0));
    CHECK(host.wait([&] { return host.output_pixel(mon, 512, 768 - 25) == 0xFFFFFF00u; }));
    // A FocusWindow while the exclusive overlay is up does not steal the keyboard.
    host.wm_do([&](WindowManager& wm) { return wm.focus(wid); });
    host.server().inject_key(48, true);
    host.server().inject_key(48, false);
    CHECK(overlay->wait_line("key 48 1", 5000));
    CHECK_EQ(w->count("key 48"), size_t(0));
    overlay->kill_now();
    CHECK(host.wait([&] { return host.server_events_of<LayerSurfaceRemoved>().size() == 1; }));
    CHECK(w->wait_count("kbenter", 2, 5000));

    // The panel goes away: the zone is returned.
    panel->kill_now();
    CHECK(host.wait([&] { return host.server_events_of<LayerSurfaceRemoved>().size() == 2; }));
    CHECK(host.wait([&] {
        auto r = host.events_of<ReservationChanged>();
        return !r.empty() && r.back().id == info.reservation && r.back().rect == Rect{};
    }));
    CHECK(host.wait([&] { return work_area(host) == screen; }));
    CHECK(host.server().layer_surfaces().empty());
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
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
    return finish("test_wl_layer");
}
