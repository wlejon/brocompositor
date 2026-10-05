// Outputs through the server role on the headless backend: wlr-randr lists
// and reconfigures them (wlr-output-management), the host sees
// OutputsChanged / MonitorsChanged with logical bounds and DPI, clients get
// the preferred buffer scale and fractional scale of the output they are on,
// add_output and mode changes regenerate the presentable image set, and an
// output turned off leaves the monitor list.
#include "linux/wl_harness.h"
#include "printers.h"

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

std::optional<std::string> randr(Host& host, std::vector<std::string> args) {
    args.insert(args.begin(), "wlr-randr");
    auto p = Child::spawn(args, host.client_env(), true);
    if (!p) return std::nullopt;
    int status = -1;
    if (!p->wait_exit(5000, &status) || status != 0) {
        std::fprintf(stderr, "wlr-randr failed (%d):\n%s\n", status, p->output().c_str());
        return std::nullopt;
    }
    return p->output();
}

std::optional<OutputInfo> info_of(Host& host, MonitorId id) {
    for (auto& o : host.server().outputs())
        if (o.id == id) return o;
    return std::nullopt;
}

void run(Host& host) {
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    MonitorId m1 = host.wm_monitors()[0].id;
    auto o1 = info_of(host, m1);
    REQUIRE(o1);
    CHECK_EQ(o1->pixel_size, (Size{800, 600}));
    CHECK_EQ(o1->layout, (Rect{0, 0, 800, 600}));
    CHECK(o1->enabled);
    CHECK_EQ(host.server().output_images(m1).size(), size_t(3));

    bool have_randr = !which("wlr-randr").empty();
    if (!have_randr) std::printf("SKIP wlr-randr parts: not installed\n");
    if (have_randr) {
        auto list = randr(host, {});
        CHECK(list && list->find(o1->name) != std::string::npos);
        CHECK(list && list->find("800x600") != std::string::npos);
    }

    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "out", "--size", "100x100"}, host.client_env());
    REQUIRE(c);
    REQUIRE(c->wait_line("ready", 5000));
    CHECK(c->wait_line("output_enter", 5000));
    CHECK(c->wait_line("fscale 120", 5000));

    // Scale 2 (through wlr-randr when available, else the host API).
    if (have_randr) {
        CHECK(randr(host, {"--output", o1->name, "--scale", "2"}).has_value());
    } else {
        OutputConfig oc;
        oc.scale = 2.0f;
        CHECK(host.server().configure_output(m1, oc));
    }
    CHECK(host.wait([&] {
        auto m = host.wm_monitors();
        return m.size() == 1 && m[0].bounds == Rect{0, 0, 400, 300} && m[0].dpi == 192;
    }));
    CHECK(c->wait_line("scale 2", 5000));
    CHECK(c->wait_line("fscale 240", 5000));
    CHECK_EQ(info_of(host, m1)->pixel_size, (Size{800, 600}));

    // Fractional 1.5 from the host: preferred buffer scale rounds up.
    OutputConfig frac;
    frac.scale = 1.5f;
    CHECK(host.server().configure_output(m1, frac));
    CHECK(c->wait_line("fscale 180", 5000));
    CHECK(host.wait([&] {
        auto m = host.wm_monitors();
        return m.size() == 1 && m[0].bounds == Rect{0, 0, 533, 400} && m[0].dpi == 144;
    }));

    // A second output joins to the right.
    MonitorId m2 = host.server().add_output(Size{640, 480});
    CHECK(m2 != kNoMonitor);
    CHECK(host.wait([&] { return host.wm_monitors().size() == 2; }));
    auto o2 = info_of(host, m2);
    REQUIRE(o2);
    CHECK_EQ(o2->layout, (Rect{533, 0, 640, 480}));
    CHECK(host.wait([&] { return host.presents(m2) > 0; }));
    if (have_randr) {
        auto list = randr(host, {});
        CHECK(list && list->find(o2->name) != std::string::npos);
    }

    // Moving the window onto it: the client enters that output (scale 1).
    WindowId w = host.window_by_app_id("out");
    REQUIRE(w != kNoWindow);
    CHECK(host.server().execute(Command{PlaceWindow{w, Rect{600, 50, 100, 100}}}));
    CHECK(c->wait_count("output_enter", 2, 5000));
    CHECK(c->wait_line("scale 1", 5000));
    CHECK(host.wait([&] { return host.server().query(w)->monitor == m2; }));

    // Mode change: a new image set at the new size.
    uint64_t gen = o2->images_generation;
    OutputConfig mode;
    mode.mode = Size{1024, 768};
    CHECK(host.server().configure_output(m2, mode));
    CHECK(host.wait([&] {
        auto i = info_of(host, m2);
        return i && i->pixel_size == Size{1024, 768} && i->images_generation != gen;
    }));
    auto imgs = host.server().output_images(m2);
    CHECK(!imgs.empty());
    for (auto& im : imgs) CHECK_EQ((Size{int32_t(im.width), int32_t(im.height)}), (Size{1024, 768}));
    uint64_t before = host.presents(m2);
    CHECK(host.wait([&] { return host.presents(m2) > before + 2; }));

    // Turning it off removes the monitor.
    if (have_randr) {
        CHECK(randr(host, {"--output", o2->name, "--off"}).has_value());
    } else {
        OutputConfig off;
        off.enabled = false;
        CHECK(host.server().configure_output(m2, off));
    }
    CHECK(host.wait([&] { return host.wm_monitors().size() == 1; }));
    CHECK(host.wait([&] {
        auto i = info_of(host, m2);
        return i && !i->enabled;
    }));
    c->kill_now();
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{800, 600};
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    run(host);
    host.stop();
    return finish("test_wl_outputs");
}
