// brodisplays against this server. On Linux brocompositor is the display
// server: it owns its outputs and publishes them (wl_output,
// wlr-output-management, wlr-gamma-control), and brodisplays, the library
// the Windows and macOS shells take their topology from, is one of its
// clients here. Checks that what brodisplays reports is what the server
// has, that a mode change and a test-then-revert made through brodisplays
// reach the server's outputs, and that its night light arrives as the
// server's gamma ramps and goes away when released or when the client
// disconnects.
#include "linux/wl_harness.h"
#include "printers.h"

#include <brodisplays/display_service.h>

#include <cstdlib>
#include <variant>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

std::optional<OutputInfo> info_of(Host& host, MonitorId id) {
    for (auto& o : host.server().outputs())
        if (o.id == id) return o;
    return std::nullopt;
}

const brodisplays::DisplayInfo* find(const brodisplays::DisplaysSnapshot& s, const std::string& id) {
    for (const auto& d : s.displays)
        if (d.id == id) return &d;
    return nullptr;
}

// Waits for a DisplaysChanged whose snapshot satisfies `pred`.
bool wait_displays(brodisplays::DisplayService& svc, const std::function<bool(const brodisplays::DisplaysSnapshot&)>& pred,
                   std::vector<brodisplays::DisplayEvent>* seen = nullptr, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        svc.events().wait_for(std::chrono::milliseconds(50));
        for (auto& ev : svc.events().drain()) {
            if (seen) seen->push_back(ev);
            if (auto* c = std::get_if<brodisplays::DisplaysChanged>(&ev))
                if (pred(c->snapshot)) return true;
        }
    }
    return false;
}

bool warm(const std::optional<GammaChanged>& g) {
    if (!g || g->ramps.size() != size_t(3) * g->size || g->size == 0) return false;
    uint16_t r = g->ramps[g->size - 1], b = g->ramps[3 * g->size - 1];
    return r > b && b < 60000;
}

bool released(const std::optional<GammaChanged>& g) { return !g || g->ramps.empty(); }

void run(Host& host) {
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    MonitorId m1 = host.wm_monitors()[0].id;
    MonitorId m2 = host.server().add_output(Size{1024, 768});
    REQUIRE(m2 != kNoMonitor);
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 2; }));
    auto o1 = info_of(host, m1), o2 = info_of(host, m2);
    REQUIRE(o1 && o2);

    for (const auto& kv : host.client_env()) {
        size_t eq = kv.find('=');
        if (eq != std::string::npos && (kv.rfind("WAYLAND_DISPLAY=", 0) == 0 || kv.rfind("XDG_RUNTIME_DIR=", 0) == 0))
            setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    }

    std::string err;
    auto svc = brodisplays::DisplayService::create({}, &err);
    REQUIRE(svc);

    std::printf("-- topology\n");
    auto snap = svc->snapshot();
    CHECK_EQ(snap.displays.size(), size_t(2));
    size_t primaries = 0;
    for (const auto& o : {*o1, *o2}) {
        const auto* d = find(snap, o.name);
        CHECK(d != nullptr);
        if (!d) continue;
        primaries += d->is_primary ? 1 : 0;
        std::printf("   %s %ux%u at %d,%d (server: %dx%d at %d,%d)\n", d->id.c_str(), d->current_mode.width,
                    d->current_mode.height, d->geometry.x, d->geometry.y, o.pixel_size.width, o.pixel_size.height,
                    o.layout.x, o.layout.y);
        CHECK_EQ(d->device_name, o.name);
        CHECK_EQ(int32_t(d->current_mode.width), o.pixel_size.width);
        CHECK_EQ(int32_t(d->current_mode.height), o.pixel_size.height);
        CHECK_EQ(d->geometry.x, o.layout.x);
        CHECK_EQ(d->geometry.y, o.layout.y);
        CHECK_EQ(int32_t(d->geometry.width), o.layout.width);
        CHECK_EQ(int32_t(d->geometry.height), o.layout.height);
        CHECK(d->is_active);
        CHECK(d->night_light.supported);
    }
    CHECK_EQ(primaries, size_t(1));

    std::printf("-- mode change through brodisplays\n");
    brodisplays::DisplayConfigChange change;
    change.display_id = o1->name;
    change.width = 1024;
    change.height = 768;
    auto res = svc->apply_configuration(change);
    CHECK(res.ok);
    if (!res.ok) std::printf("   %s\n", res.error.c_str());
    CHECK(host.wait([&] {
        auto i = info_of(host, m1);
        return i && i->pixel_size == Size{1024, 768};
    }));
    CHECK(wait_displays(*svc, [&](const brodisplays::DisplaysSnapshot& s) {
        const auto* d = find(s, o1->name);
        return d && d->current_mode.width == 1024 && d->current_mode.height == 768;
    }));

    std::printf("-- test-then-revert through brodisplays\n");
    change.width = 640;
    change.height = 480;
    res = svc->apply_temporary_configuration(change, std::chrono::milliseconds(1500));
    CHECK(res.ok);
    CHECK(host.wait([&] {
        auto i = info_of(host, m1);
        return i && i->pixel_size == Size{640, 480};
    }));
    std::vector<brodisplays::DisplayEvent> seen;
    CHECK(wait_displays(
        *svc,
        [&](const brodisplays::DisplaysSnapshot& s) {
            const auto* d = find(s, o1->name);
            return d && d->current_mode.width == 1024 && d->current_mode.height == 768;
        },
        &seen, 6000));
    CHECK(host.wait([&] {
        auto i = info_of(host, m1);
        return i && i->pixel_size == Size{1024, 768};
    }));
    // Once no longer pending, the ConfigurationReverted event is queued.
    CHECK(host.wait([&] { return !svc->is_revert_pending(); }));
    for (auto& ev : svc->events().drain()) seen.push_back(ev);
    bool reverted = false;
    for (const auto& ev : seen)
        if (auto* r = std::get_if<brodisplays::ConfigurationReverted>(&ev)) reverted = r->restored;
    CHECK(reverted);

    std::printf("-- night light as server gamma\n");
    res = svc->set_night_light(true, 3400);
    CHECK(res.ok);
    if (!res.ok) std::printf("   %s\n", res.error.c_str());
    CHECK(host.wait([&] { return warm(host.server().gamma(m1)) && warm(host.server().gamma(m2)); }));
    snap = svc->snapshot();
    for (const auto& d : snap.displays) {
        CHECK(d.night_light.enabled);
        CHECK_EQ(d.night_light.temperature_kelvin, 3400u);
    }
    CHECK(host.wait([&] { return !host.server_events_of<GammaChanged>().empty(); }));
    res = svc->set_night_light(false, 0);
    CHECK(res.ok);
    CHECK(host.wait([&] { return released(host.server().gamma(m1)) && released(host.server().gamma(m2)); }));

    // Left on when the client goes away: the server drops the ramps.
    res = svc->set_night_light(true, 3000);
    CHECK(res.ok);
    CHECK(host.wait([&] { return warm(host.server().gamma(m1)); }));
    svc.reset();
    CHECK(host.wait([&] { return released(host.server().gamma(m1)) && released(host.server().gamma(m2)); }));
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    // brodisplays would drive a desktop's own night light found on the
    // session bus (KWin, GNOME); this test is about this server only, so
    // there is no session bus and no desktop.
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/brocompositor-test-bus", 1);
    unsetenv("XDG_CURRENT_DESKTOP");
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
    return finish("test_wl_brodisplays");
}
