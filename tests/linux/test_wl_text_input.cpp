// text-input-v3 <-> input-method-v2 relay: the input method (a minimal IME
// client) is activated for the focused text field with its surrounding
// text, its preedit and commit strings reach the field, its keyboard grab
// takes the routed keys (never while the session is locked), its popup
// (candidate window) joins the focused window's tree under the cursor
// rectangle, focus changes deactivate it, and a second input method is
// refused. foot (when installed) gets real text committed by the IME.
#include "linux/wl_harness.h"
#include "printers.h"

#include <fstream>

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

void relay(Host& host) {
    auto ime = Child::spawn({BC_WL_SESSION_CLIENT, "--ime"}, host.client_env());
    REQUIRE(ime);
    REQUIRE(ime->wait_line("im_ready", 5000));
    auto field = Child::spawn({BC_WL_INPUT_CLIENT, "--app-id", "field", "--text-input"}, host.client_env());
    REQUIRE(field);
    REQUIRE(field->wait_line("tienter", 5000));
    CHECK(ime->wait_line("im_activate", 5000));
    CHECK(ime->wait_line("im_surrounding hello", 5000));

    ime->send("preedit ni\n");
    CHECK(field->wait_line("preedit ni", 5000));
    CHECK(field->wait_line("tidone", 5000));
    ime->send("commit 你好\n");
    CHECK(field->wait_line("commit 你好", 5000));

    // The keyboard grab takes routed keys away from the field.
    ime->send("grab\n");
    CHECK(ime->wait_line("im_grabbed", 5000));
    usleep(200 * 1000);
    tap_key(host.server(), kKeyA);
    CHECK(ime->wait_line("im_key 30 1", 5000));
    CHECK(ime->wait_line("im_key 30 0", 5000));
    CHECK_EQ(field->count("key 30"), size_t(0));

    // The candidate popup: placed under the cursor rectangle (20,30 2x16).
    ime->send("popup\n");
    CHECK(ime->wait_line("im_popup_rect", 5000));
    WindowId w = host.window_by_app_id("field");
    CHECK(host.wait([&] {
        for (const SurfaceNode& n : host.server().window_surfaces(w))
            if (n.popup && n.offset == Point{20, 46} && n.size == Size{60, 20}) return true;
        return false;
    }));

    // Locked: the grab is bypassed, keys reach only the lock surface.
    auto locker = Child::spawn({BC_WL_SESSION_CLIENT, "--lock"}, host.client_env());
    REQUIRE(locker);
    REQUIRE(locker->wait_line("locked", 5000));
    REQUIRE(locker->wait_line("kbenter", 5000));
    size_t im_keys = ime->count("im_key");
    tap_key(host.server(), kKeyB);
    CHECK(locker->wait_line("key 48 1", 5000));
    CHECK_EQ(ime->count("im_key"), im_keys);
    CHECK(host.server().window_surfaces(w).empty());
    locker->send("unlock\n");
    CHECK(locker->wait_line("unlocked", 5000));
    CHECK(host.wait([&] { return host.server().session_lock_state() == LockState::Unlocked; }));
    tap_key(host.server(), kKeyC);
    CHECK(ime->wait_line("im_key 46 1", 5000));

    // Focus moves to a window without text input: the IME deactivates.
    auto other = Child::spawn({BC_WL_INPUT_CLIENT, "--app-id", "plain"}, host.client_env());
    REQUIRE(other);
    CHECK(other->wait_line("kbenter", 5000));
    CHECK(field->wait_line("tileave", 5000));
    CHECK(ime->wait_line("im_deactivate", 5000));

    // One input method per seat.
    auto ime2 = Child::spawn({BC_WL_SESSION_CLIENT, "--ime"}, host.client_env());
    REQUIRE(ime2);
    CHECK(ime2->wait_line("im_unavailable", 5000));
}

// foot speaks text-input-v3: text the IME commits lands in its shell.
void foot(Host& host, const std::string& dir) {
    if (which("foot").empty()) return (void)std::printf("SKIP foot: not installed\n");
    auto ime = Child::spawn({BC_WL_SESSION_CLIENT, "--ime"}, host.client_env());
    REQUIRE(ime);
    REQUIRE(ime->wait_line("im_ready", 5000));
    std::string file = dir + "/ime.txt";
    auto env = host.client_env();
    env.push_back("SHELL=/bin/sh");
    env.push_back("ENV=");
    auto t = Child::spawn({"foot", "--app-id", "bc-foot-ime", "-o", "main.shell=/bin/sh", "/bin/sh"}, env, true);
    REQUIRE(t);
    REQUIRE(host.wait([&] { return host.window_by_app_id("bc-foot-ime") != kNoWindow; }, 10000));
    CHECK(ime->wait_line("im_activate", 10000));
    usleep(300 * 1000);
    ime->send("commit echo ime-ok > " + file + "\n");
    usleep(200 * 1000);
    tap_key(host.server(), kKeyEnter);
    std::string got;
    CHECK(host.wait(
        [&] {
            std::ifstream in(file);
            std::getline(in, got);
            return got == "ime-ok";
        },
        10000));
    CHECK_EQ(got, std::string("ime-ok"));
    host.server().close(host.window_by_app_id("bc-foot-ime"));
    t->wait_exit(5000);
}

}  // namespace

int main() {
    std::string dir = private_runtime_dir();
    if (dir.empty()) return 1;
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
    relay(host);
    foot(host, dir);
    host.stop();
    return finish("test_wl_text_input");
}
