// Frame pacing through the server role: an animating client (a new buffer
// per frame callback) runs at the headless output's 60 Hz because callbacks
// are sent only for surfaces the host drew in a present; presentation-time
// feedback reports them presented; OutputPresented carries monotonic
// timestamps and the refresh period; buffers are released and reused (the
// image set stays small); a hidden window, or a host that stops presenting,
// gets no callbacks until drawing resumes.
#include "linux/wl_harness.h"
#include "printers.h"

#include <thread>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The fewest presents per second the test host must manage. It composites on
// the CPU; an unoptimized build (Debug, coverage) on a shared runner can fall
// below the 60 Hz output, and there the frames only have to keep coming. The
// ceiling (no unthrottled presents) holds in every build.
#if defined(__OPTIMIZE__)
constexpr uint64_t kMinPresentsPerSecond = 45;
#else
constexpr uint64_t kMinPresentsPerSecond = 20;
#endif

// Frames the client counted over `ms`, and presents the host made.
std::pair<size_t, uint64_t> measure(Host& host, Child& c, MonitorId mon, int ms) {
    size_t f0 = c.count("frame ");
    uint64_t p0 = host.presents(mon);
    sleep_ms(ms);
    return {c.count("frame ") - f0, host.presents(mon) - p0};
}

void run(Host& host) {
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    MonitorId mon = host.wm_monitors()[0].id;

    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "anim", "--size", "120x90", "--animate"}, host.client_env());
    REQUIRE(c);
    REQUIRE(c->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("anim") != kNoWindow; }));
    WindowId w = host.window_by_app_id("anim");
    SurfaceId root = SurfaceId(host.server().query(w)->native);
    CHECK(c->wait_count("frame ", 10, 5000));

    // ~60 callbacks per second, one per present that drew the surface.
    auto [frames, presents] = measure(host, *c, mon, 1000);
    std::printf("1 s: %zu frame callbacks, %llu presents\n", frames, static_cast<unsigned long long>(presents));
    CHECK(presents >= kMinPresentsPerSecond && presents <= 75);
    CHECK(frames + 3 >= presents && frames <= presents + 1);
    CHECK(c->count("presented ") + 5 >= c->count("frame "));

    // OutputPresented: presented, monotonic timestamps, ~16.7 ms refresh.
    auto pres = host.server_events_of<OutputPresented>();
    CHECK(pres.size() >= 30);
    int64_t last = 0;
    bool monotonic = true, all_presented = true, refresh_ok = true;
    for (auto& p : pres) {
        if (p.output != mon) continue;
        if (!p.presented) all_presented = false;
        if (p.timestamp_ns < last) monotonic = false;
        last = p.timestamp_ns;
        if (p.refresh_ns && (p.refresh_ns < 15000000 || p.refresh_ns > 18000000)) refresh_ok = false;
    }
    CHECK(monotonic);
    CHECK(all_presented);
    CHECK(refresh_ok);

    // Buffers come back: the client's pool (and so our image set) stays small.
    auto src = host.server().surface(root);
    REQUIRE(src);
    CHECK(src->images().size() <= 4);
    CHECK(src->state().size == (Size{120, 90}));

    // Hidden: no longer drawn, so no callbacks.
    CHECK(host.server().execute(Command{SetWindowVisible{w, false}}));
    CHECK(host.wait([&] { return !host.last_drawn(mon).count(root); }));
    sleep_ms(100);
    auto hidden = measure(host, *c, mon, 500);
    CHECK(hidden.first <= 1);
    CHECK(hidden.second >= kMinPresentsPerSecond / 2 - 2);  // the host keeps presenting the output
    CHECK(host.server().execute(Command{SetWindowVisible{w, true}}));
    size_t before = c->count("frame ");
    CHECK(c->wait_count("frame ", before + 10, 5000));

    // A host that stops presenting stops the callbacks too.
    host.set_presenting(false);
    sleep_ms(100);
    auto idle = measure(host, *c, mon, 500);
    CHECK_EQ(idle.second, uint64_t(0));
    CHECK(idle.first <= 1);
    host.set_presenting(true);
    before = c->count("frame ");
    CHECK(c->wait_count("frame ", before + 10, 5000));

    // A static client gets one callback per commit, not one per present.
    auto s = Child::spawn({BC_WL_CLIENT, "--app-id", "static", "--size", "50x50"}, host.client_env());
    REQUIRE(s);
    CHECK(s->wait_line("frame 1", 5000));
    sleep_ms(300);
    CHECK_EQ(s->count("frame "), size_t(1));
    s->send("redraw\n");
    CHECK(s->wait_line("frame 2", 5000));
    sleep_ms(200);
    CHECK_EQ(s->count("frame "), size_t(2));
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{640, 480};
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    run(host);
    host.stop();
    return finish("test_wl_frames");
}
