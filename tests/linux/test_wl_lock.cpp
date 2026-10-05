// ext-session-lock-v1: the lock is the server's to enforce, whatever the
// host and the other clients do.
//
//   * hostile: a window client with keyboard + pointer focus and a fresh
//     input serial is locked out: no keys, no pointer, no popup grab, its
//     surface refuses acquire() and its tree is empty, so the presented
//     output shows only the lock surface, even through a naive host that
//     keeps drawing windows; a second locker is refused; killing the lock
//     client leaves the session locked (Abandoned) until a new lock client
//     takes over and unlocks, which gives the window its focus back;
//   * swaylock (when installed) locks and draws, and dying does not unlock.
#include "linux/wl_harness.h"
#include "printers.h"

#include <signal.h>
#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

#ifndef BC_WL_INPUT_CLIENT
#error BC_WL_INPUT_CLIENT must name the input test client
#endif

constexpr uint32_t kVictim = 0xFFFF0000, kLocker = 0xFF10C010, kLocker2 = 0xFF1010C0;

bool start(Host& host, bool lock_aware) {
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{800, 600};
    o.server.xwayland = XwaylandMode::Off;
    o.lock_aware = lock_aware;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return false;
    }
    return true;
}

MonitorId first_output(Host& host) {
    auto outs = host.server().outputs();
    return outs.empty() ? kNoMonitor : outs.front().id;
}

std::unique_ptr<Child> locker(Host& host, uint32_t color) {
    char c[16];
    std::snprintf(c, sizeof c, "%08X", color);
    return Child::spawn({BC_WL_SESSION_CLIENT, "--lock", "--color", c}, host.client_env());
}

LockState lock_state(Host& host) { return host.server().session_lock_state(); }

// The presented pixel at a layout point, once frames that started after now
// were presented.
std::optional<uint32_t> fresh_pixel(Host& host, MonitorId out, int x, int y) {
    uint64_t base = host.presents(out);
    host.server().schedule_frame(out);
    host.wait([&] { return host.presents(out) >= base + 2; });
    return host.output_pixel(out, uint32_t(x), uint32_t(y));
}

void hostile(bool lock_aware) {
    std::printf("-- hostile (lock-aware host: %s)\n", lock_aware ? "yes" : "no");
    Host host;
    REQUIRE(start(host, lock_aware));
    MonitorId out = first_output(host);
    auto victim = Child::spawn({BC_WL_INPUT_CLIENT, "--app-id", "victim", "--size", "300x200", "--color", "FFFF0000"},
                               host.client_env());
    REQUIRE(victim);
    REQUIRE(victim->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("victim") != kNoWindow; }));
    WindowId w = host.window_by_app_id("victim");
    REQUIRE(victim->wait_line("kbenter", 5000));
    Rect f = host.server().query(w)->frame;
    SurfaceId root = SurfaceId(host.server().query(w)->native);
    int px = f.x + 150, py = f.y + 120;  // clear of the popup the victim tries at (10, 10)
    host.server().inject_pointer_warp(px, py);
    REQUIRE(victim->wait_line("enter", 5000));
    tap_key(host.server(), kKeyA);
    REQUIRE(victim->wait_line("key 30 1", 5000));
    CHECK(host.wait([&] { return host.output_pixel(out, uint32_t(px), uint32_t(py)) == kVictim; }));

    // ---- lock
    auto l1 = locker(host, kLocker);
    REQUIRE(l1);
    CHECK(l1->wait_line("locked", 5000));
    CHECK_EQ(lock_state(host), LockState::Locked);
    CHECK(host.wait([&] {
        for (auto& e : host.server_events_of<SessionLockChanged>())
            if (e.state == LockState::Locked) return true;
        return false;
    }));
    CHECK(victim->wait_line("kbleave", 5000));
    CHECK(victim->wait_line("leave", 5000));
    CHECK(l1->wait_line("kbenter", 5000));

    // Keys and pointer reach only the lock surface.
    size_t vkeys = victim->count("key "), venters = victim->count("enter");
    tap_key(host.server(), kKeyB);
    CHECK(l1->wait_line("key 48 1", 5000));
    host.server().inject_pointer_warp(px + 5, py + 5);
    CHECK(l1->wait_line("enter", 5000));
    // Host-side routing straight at the victim's surface is refused too.
    host.server().pointer_route(root, 10, 10, 0);
    host.server().keyboard_key(0, kKeyC, true, KeyboardModifiers{});
    host.server().keyboard_key(0, kKeyC, false, KeyboardModifiers{});
    CHECK(l1->wait_line("key 46 1", 5000));

    // A popup grab with the serial of a key it got before the lock: the grab
    // ends, the popup never takes the keyboard.
    victim->send("grabpopup\n");
    usleep(300 * 1000);
    tap_key(host.server(), kKeyA);
    CHECK(l1->wait_line("key 30 1", 5000));
    CHECK_EQ(victim->count("key "), vkeys);
    CHECK_EQ(victim->count("enter"), venters);
    CHECK_EQ(victim->count("kbenter"), size_t(1));

    // Nothing of the victim is handed to the host.
    auto src = host.server().surface(root);
    CHECK(!src || !src->acquire());
    CHECK(host.server().window_surfaces(w).empty());
    CHECK(!host.server().hit_test(w, 20, 20));
    CHECK(host.server().unmanaged_surfaces().empty());
    std::optional<uint32_t> p = fresh_pixel(host, out, px, py);
    CHECK(p.has_value() && *p != kVictim);
    if (lock_aware) CHECK_EQ(p.value_or(0), kLocker);

    // A second lock client is refused while the first holds the lock.
    auto l2 = locker(host, kLocker2);
    REQUIRE(l2);
    CHECK(l2->wait_line("finished", 5000));

    // ---- the lock client dies: still locked
    kill(l1->pid(), SIGKILL);
    CHECK(l1->wait_exit(5000));
    CHECK(host.wait([&] { return lock_state(host) == LockState::Abandoned; }));
    tap_key(host.server(), kKeyA);
    usleep(200 * 1000);
    CHECK_EQ(victim->count("key "), vkeys);
    CHECK_EQ(victim->count("kbenter"), size_t(1));
    p = fresh_pixel(host, out, px, py);
    CHECK(p.has_value() && *p != kVictim);
    CHECK(host.server().window_surfaces(w).empty());

    // ---- a new lock client takes over, then unlocks
    auto l3 = locker(host, kLocker2);
    REQUIRE(l3);
    CHECK(l3->wait_line("locked", 5000));
    CHECK_EQ(lock_state(host), LockState::Locked);
    if (lock_aware) CHECK(host.wait([&] { return fresh_pixel(host, out, px, py) == kLocker2; }));
    l3->send("unlock\n");
    CHECK(l3->wait_line("unlocked", 5000));
    CHECK(host.wait([&] { return lock_state(host) == LockState::Unlocked; }));
    CHECK(victim->wait_count("kbenter", 2, 5000));
    tap_key(host.server(), kKeyC);
    CHECK(victim->wait_line("key 46 1", 5000));
    CHECK(!host.server().window_surfaces(w).empty());
    CHECK(host.wait([&] { return fresh_pixel(host, out, px, py) == kVictim; }));
    host.stop();
}

void swaylock() {
    if (which("swaylock").empty()) {
        std::printf("SKIP swaylock: not installed\n");
        return;
    }
    std::printf("-- swaylock\n");
    Host host;
    REQUIRE(start(host, true));
    MonitorId out = first_output(host);
    auto victim = Child::spawn({BC_WL_INPUT_CLIENT, "--app-id", "victim", "--size", "300x200", "--color", "FFFF0000"},
                               host.client_env());
    REQUIRE(victim);
    REQUIRE(victim->wait_line("kbenter", 5000));
    auto env = host.client_env();
    env.push_back("HOME=" + host.server().runtime_dir());  // no user config
    auto sl = Child::spawn({"swaylock", "--no-unlock-indicator", "-c", "00ff00"}, env, true);
    REQUIRE(sl);
    CHECK(host.wait([&] { return lock_state(host) == LockState::Locked; }, 10000));
    CHECK(host.wait([&] { return !host.server().lock_surface_tree(out).empty(); }, 10000));
    CHECK(victim->wait_line("kbleave", 5000));
    CHECK(host.wait([&] { return fresh_pixel(host, out, 400, 300) == 0xFF00FF00u; }, 10000));
    tap_key(host.server(), kKeyA);  // goes to swaylock's password buffer, not the window
    usleep(200 * 1000);
    CHECK_EQ(victim->count("key "), size_t(0));
    // swaylock dies: the session stays locked.
    kill(sl->pid(), SIGKILL);
    CHECK(sl->wait_exit(5000));
    CHECK(host.wait([&] { return lock_state(host) == LockState::Abandoned; }));
    CHECK(host.server().lock_surface_tree(out).empty());
    auto p = fresh_pixel(host, out, 400, 300);
    CHECK(p.has_value() && *p != 0xFFFF0000u);
    auto l = locker(host, kLocker);
    REQUIRE(l);
    CHECK(l->wait_line("locked", 5000));
    l->send("unlock\n");
    CHECK(host.wait([&] { return lock_state(host) == LockState::Unlocked; }));
    CHECK(victim->wait_count("kbenter", 2, 5000));
    host.stop();
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    hostile(true);
    hostile(false);
    swaylock();
    return finish("test_wl_lock");
}
