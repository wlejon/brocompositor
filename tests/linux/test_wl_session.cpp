// Desktop-session plumbing seen by real clients: swayidle idles and resumes
// on ext-idle-notify, a visible idle inhibitor keeps it from idling; wlsunset
// sets gamma ramps through wlr-gamma-control that reach the host (headless
// outputs have no hardware LUT) and are restored when it exits; a Wayland
// window larger than its output is configured down to the work area.
#include "linux/wl_harness.h"
#include "printers.h"

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

void idle(Host& host) {
    if (which("swayidle").empty()) return (void)std::printf("SKIP swayidle: not installed\n");
    std::printf("-- swayidle (ext-idle-notify)\n");
    auto si = Child::spawn({"swayidle", "-w", "timeout", "1", "echo idled", "resume", "echo resumed"},
                           host.client_env());
    REQUIRE(si);
    CHECK(si->wait_line("idled", 5000));
    tap_key(host.server(), kKeyA);
    CHECK(si->wait_line("resumed", 5000));
    // Host-side activity (not device input) also counts.
    CHECK(si->wait_count("idled", 2, 5000));
    host.server().notify_activity();
    CHECK(si->wait_count("resumed", 2, 5000));

    std::printf("-- idle-inhibit\n");
    auto c = Child::spawn({BC_WL_INPUT_CLIENT, "--app-id", "video", "--idle-inhibit"}, host.client_env());
    REQUIRE(c);
    REQUIRE(c->wait_line("ready", 5000));
    CHECK(host.wait([&] { return host.server().idle_inhibited(); }));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<IdleInhibitChanged>();
        return !ev.empty() && ev.back().inhibited;
    }));
    // Wake from the current idle, then stay awake while inhibited.
    tap_key(host.server(), kKeyB);
    size_t idled = si->count("idled");
    usleep(2500 * 1000);
    CHECK_EQ(si->count("idled"), idled);
    // The inhibitor's window goes away: idling resumes.
    c->send("quit\n");
    CHECK(c->wait_exit(5000));
    CHECK(host.wait([&] { return !host.server().idle_inhibited(); }));
    CHECK(si->wait_count("idled", idled + 1, 5000));
    si->kill_now();
}

void gamma(Host& host) {
    if (which("wlsunset").empty()) return (void)std::printf("SKIP wlsunset: not installed\n");
    std::printf("-- wlsunset (wlr-gamma-control)\n");
    MonitorId out = host.wm_monitors().at(0).id;
    // -t 4000 -T 4001: whatever the time of day, about 4000 K.
    auto ws = Child::spawn({"wlsunset", "-t", "4000", "-T", "4001"}, host.client_env(), true);
    REQUIRE(ws);
    CHECK(host.wait(
        [&] {
            auto g = host.server().gamma(out);
            return g && !g->ramps.empty();
        },
        10000));
    auto g = host.server().gamma(out);
    REQUIRE(g && !g->ramps.empty());
    CHECK_EQ(g->size, 256u);
    CHECK_EQ(g->ramps.size(), size_t(3 * 256));
    CHECK(!g->hardware);
    uint16_t r = g->ramps[255], b = g->ramps[2 * 256 + 255];
    CHECK(r > b);         // warm: blue cut more than red
    CHECK(b < 60000);     // not the identity ramp
    // The mirror gamma() reads is updated before the event reaches the
    // host's queue, so the event may land a moment later.
    CHECK(host.wait([&] { return !host.server_events_of<GammaChanged>().empty(); }));
    ws->kill_now();
    CHECK(ws->wait_exit(5000));
    CHECK(host.wait([&] {
        auto now = host.server().gamma(out);
        return !now || now->ramps.empty();
    }));
    auto ev = host.server_events_of<GammaChanged>();
    CHECK(!ev.empty() && ev.back().ramps.empty());
}

void oversize(Host& host) {
    std::printf("-- oversize Wayland window\n");
    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "huge", "--size", "3000x2000"}, host.client_env());
    REQUIRE(c);
    REQUIRE(c->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("huge") != kNoWindow; }));
    WindowId w = host.window_by_app_id("huge");
    Rect area = host.wm_monitors().at(0).work_area;
    std::string want = "configure " + std::to_string(area.width) + " " + std::to_string(area.height);
    CHECK(c->wait_line(want, 5000));
    CHECK(host.wait([&] {
        auto s = host.server().query(w);
        return s && s->frame == area;
    }));
    auto s = host.server().query(w);
    REQUIRE(s);
    CHECK_EQ(s->frame, area);
    c->send("quit\n");
    c->wait_exit(5000);
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.xwayland = XwaylandMode::Off;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    oversize(host);
    gamma(host);
    idle(host);
    host.stop();
    return finish("test_wl_session");
}
