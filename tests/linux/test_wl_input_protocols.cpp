// The newer input protocols through the server role, each seen by a real
// client: wl_touch (host-routed touch points), tablet-v2 (tool proximity /
// motion / pressure / tip / button, pad buttons on the focused surface),
// pointer constraints with relative pointer (lock: the cursor stays, deltas
// flow; confine: the cursor stays in the region), keyboard-shortcuts-inhibit,
// and virtual input from wtype (virtual-keyboard) and wlrctl (virtual-pointer).
#include "linux/wl_harness.h"
#include "printers.h"

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

constexpr uint32_t kBtnLeft = 272, kBtnStylus = 331;

std::unique_ptr<Child> window(Host& host, const std::string& app_id, std::vector<std::string> extra = {}) {
    std::vector<std::string> argv = {BC_WL_INPUT_CLIENT, "--app-id", app_id, "--size", "200x150"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    auto c = Child::spawn(argv, host.client_env());
    if (!c || !c->wait_line("ready", 5000)) return nullptr;
    if (!host.wait([&] { return host.window_by_app_id(app_id) != kNoWindow; })) return nullptr;
    return c;
}

Rect frame_of(Host& host, const std::string& app_id) {
    auto s = host.server().query(host.window_by_app_id(app_id));
    return s ? s->frame : Rect{};
}

void touch(Host& host) {
    std::printf("-- touch\n");
    auto c = window(host, "touch");
    REQUIRE(c);
    Rect f = frame_of(host, "touch");
    auto& s = host.server();
    // The virtual touchscreen appears with its first event; give the client
    // time to see the touch capability and create its wl_touch.
    s.inject_touch_frame();
    usleep(300 * 1000);
    s.inject_touch_down(0, f.x + 30, f.y + 40);
    s.inject_touch_frame();
    CHECK(c->wait_line("tdown 0 30 40", 5000));
    CHECK(c->wait_line("tframe", 5000));
    s.inject_touch_down(1, f.x + 100, f.y + 60);
    s.inject_touch_motion(0, f.x + 35, f.y + 45);
    s.inject_touch_frame();
    CHECK(c->wait_line("tdown 1 100 60", 5000));
    CHECK(c->wait_line("tmotion 0 35 45", 5000));
    s.inject_touch_up(0);
    s.inject_touch_up(1);
    s.inject_touch_frame();
    CHECK(c->wait_line("tup 0", 5000));
    CHECK(c->wait_line("tup 1", 5000));
    CHECK(host.wait([&] { return !host.server_events_of<TouchDown>().empty(); }));
    // A touch outside every surface goes nowhere.
    s.inject_touch_down(2, 1000, 1000);
    s.inject_touch_up(2);
    s.inject_touch_frame();
    usleep(200 * 1000);
    CHECK_EQ(c->count("tdown 2"), size_t(0));
}

void tablet(Host& host) {
    std::printf("-- tablet\n");
    auto c = window(host, "tablet");
    REQUIRE(c);
    REQUIRE(c->wait_line("kbenter", 5000));
    Rect f = frame_of(host, "tablet");
    auto& s = host.server();
    s.inject_tablet_proximity(f.x + 20, f.y + 30, true);
    CHECK(c->wait_line("tool_in", 5000));
    CHECK(c->wait_line("pad_enter", 5000));  // the pad follows keyboard focus
    s.inject_tablet_motion(f.x + 50, f.y + 60, 0.5);
    CHECK(c->wait_line("tool_motion 50 60", 5000));
    CHECK(c->wait_line("tool_pressure 500", 5000));
    s.inject_tablet_tip(true);
    CHECK(c->wait_line("tool_down", 5000));
    s.inject_tablet_tip(false);
    CHECK(c->wait_line("tool_up", 5000));
    s.inject_tablet_button(kBtnStylus, true);
    CHECK(c->wait_line("tool_button 331 1", 5000));
    s.inject_tablet_button(kBtnStylus, false);
    s.inject_tablet_pad_button(2, true);
    CHECK(c->wait_line("pad_button 2 1", 5000));
    s.inject_tablet_pad_button(2, false);
    s.inject_tablet_proximity(f.x + 50, f.y + 60, false);
    CHECK(c->wait_line("tool_out", 5000));
    CHECK(host.wait([&] { return !host.server_events_of<TabletToolProximity>().empty(); }));
}

void constraints(Host& host) {
    std::printf("-- pointer lock / confine + relative pointer\n");
    auto& s = host.server();
    auto c = window(host, "locker", {"--lock-pointer", "--relative"});
    REQUIRE(c);
    Rect f = frame_of(host, "locker");
    s.inject_pointer_warp(f.x + 60, f.y + 70);
    CHECK(c->wait_line("enter 60 70", 5000));
    CHECK(c->wait_line("locked", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<PointerConstraintChanged>();
        return !ev.empty() && ev.back().kind == PointerConstraintKind::Locked;
    }));
    s.inject_pointer_motion(10, 5);
    CHECK(c->wait_line("rel 10 5", 5000));
    usleep(100 * 1000);
    CHECK(s.cursor_position() == std::make_pair(double(f.x + 60), double(f.y + 70)));
    CHECK_EQ(c->count("motion "), size_t(0));
    // Leaving the surface (a host warp) ends the lock.
    s.inject_pointer_warp(5, 5);
    CHECK(c->wait_line("unlocked", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<PointerConstraintChanged>();
        return !ev.empty() && ev.back().kind == PointerConstraintKind::None;
    }));
    c->send("quit\n");
    CHECK(c->wait_exit(5000));

    auto d = window(host, "confiner", {"--confine-pointer"});
    REQUIRE(d);
    f = frame_of(host, "confiner");
    s.inject_pointer_warp(f.x + 10, f.y + 10);
    CHECK(d->wait_line("confined", 5000));
    s.inject_pointer_motion(300, 300);
    usleep(100 * 1000);
    auto p = s.cursor_position();
    CHECK(p.first < f.x + 50 && p.second < f.y + 50);
    CHECK(p.first >= f.x + 10 && p.second >= f.y + 10);
    d->send("quit\n");
    CHECK(d->wait_exit(5000));
}

void shortcuts(Host& host) {
    std::printf("-- keyboard-shortcuts-inhibit\n");
    auto c = window(host, "inhibitor", {"--inhibit-shortcuts"});
    REQUIRE(c);
    CHECK(c->wait_line("inhibit_active", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<ShortcutsInhibitChanged>();
        return !ev.empty() && ev.back().active;
    }));
    tap_key(host.server(), kKeyA);
    CHECK(c->wait_line("key 30 1", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<KeyboardKey>();
        return !ev.empty() && ev.back().shortcuts_inhibited;
    }));
    auto other = window(host, "other");
    REQUIRE(other);
    CHECK(c->wait_line("inhibit_inactive", 5000));
    tap_key(host.server(), kKeyB);
    CHECK(other->wait_line("key 48 1", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<KeyboardKey>();
        return !ev.empty() && !ev.back().shortcuts_inhibited;
    }));
}

void virtual_input(Host& host) {
    auto c = window(host, "virtual");
    REQUIRE(c);
    REQUIRE(c->wait_line("kbenter", 5000));
    if (which("wtype").empty()) {
        std::printf("SKIP wtype: not installed\n");
    } else {
        std::printf("-- wtype\n");
        auto w = Child::spawn({"wtype", "abc"}, host.client_env(), true);
        REQUIRE(w);
        CHECK(w->wait_exit(5000));
        // wtype brings its own keymap (its own keycodes): the client sees
        // three presses, the host the keysyms a, b, c.
        CHECK(c->wait_count("key ", 6, 5000));
        CHECK(host.wait([&] {
            std::string typed;
            for (auto& k : host.server_events_of<KeyboardKey>())
                if (k.pressed && k.keysym >= 0x61 && k.keysym <= 0x7a) typed += char(k.keysym);
            return typed.find("abc") != std::string::npos;
        }));
    }
    if (which("wlrctl").empty()) {
        std::printf("SKIP wlrctl: not installed\n");
    } else {
        std::printf("-- wlrctl pointer\n");
        Rect f = frame_of(host, "virtual");
        host.server().inject_pointer_warp(f.x + 20, f.y + 20);
        CHECK(c->wait_line("enter 20 20", 5000));
        auto m = Child::spawn({"wlrctl", "pointer", "move", "15", "10"}, host.client_env(), true);
        REQUIRE(m);
        CHECK(m->wait_exit(5000));
        CHECK(c->wait_line("motion 35 30", 5000));
        auto k = Child::spawn({"wlrctl", "pointer", "click", "left"}, host.client_env(), true);
        REQUIRE(k);
        CHECK(k->wait_exit(5000));
        CHECK(c->wait_line("button 272 1", 5000));
        CHECK(c->wait_line("button 272 0", 5000));
    }
    (void)kBtnLeft;
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{1024, 1024};  // normalized device coordinates map back exactly
    o.server.xwayland = XwaylandMode::Off;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    touch(host);
    tablet(host);
    constraints(host);
    shortcuts(host);
    virtual_input(host);
    host.stop();
    return finish("test_wl_input_protocols");
}
