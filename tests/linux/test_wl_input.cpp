// Input through the server role on the headless backend: virtual pointer and
// keyboard devices feed the server, the host hit-tests and routes (Host's
// route_pointer / keyboard_key), and real clients see it: the scripted
// client reports enter/motion/button/axis/key with surface-local
// coordinates, keyboard focus follows FocusWindow, the cursor clamps to the
// output layout, and foot (when installed) runs a shell command typed
// through the keyboard.
#include "linux/wl_harness.h"
#include "printers.h"

#include <fstream>
#include <sstream>

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

constexpr uint32_t kBtnLeft = 272;

std::vector<std::string> client_args(const std::string& app_id) {
    return {BC_WL_CLIENT, "--app-id", app_id, "--size", "200x150", "--color", "FF00FF00"};
}

void type_keys(Host& host, const std::vector<uint32_t>& keys) {
    for (uint32_t k : keys) {
        host.server().inject_key(k, true);
        host.server().inject_key(k, false);
    }
}

void scripted(Host& host) {
    auto a = Child::spawn(client_args("in-a"), host.client_env());
    REQUIRE(a);
    REQUIRE(a->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("in-a") != kNoWindow; }));
    WindowId wa = host.window_by_app_id("in-a");
    REQUIRE(host.wait([&] { return host.wm_focused() == wa; }));
    REQUIRE(a->wait_line("kbenter", 5000));
    Rect f = host.server().query(wa)->frame;

    // Pointer: warp in, move, click, scroll; surface-local coordinates.
    host.server().inject_pointer_warp(f.x + 30, f.y + 40);
    CHECK(a->wait_line("enter 30 40", 5000));
    host.server().inject_pointer_motion(5, 6);
    CHECK(a->wait_line("motion 35 46", 5000));
    auto pos = host.server().cursor_position();
    CHECK_EQ(pos.first, double(f.x + 35));
    CHECK_EQ(pos.second, double(f.y + 46));
    host.server().inject_pointer_button(kBtnLeft, true);
    host.server().inject_pointer_button(kBtnLeft, false);
    CHECK(a->wait_line("button 272 1", 5000));
    CHECK(a->wait_line("button 272 0", 5000));
    host.server().inject_pointer_axis(0, 15, 120);  // one wheel click
    CHECK(a->wait_line("axis 0 15", 5000));
    CHECK(host.wait([&] { return !host.server_events_of<PointerAxis>().empty(); }));

    // Keyboard: KEY_A reaches the focused client; the server event carries
    // the xkb keysym.
    type_keys(host, {30});
    CHECK(a->wait_line("key 30 1", 5000));
    CHECK(a->wait_line("key 30 0", 5000));
    CHECK(host.wait([&] {
        for (auto& k : host.server_events_of<KeyboardKey>())
            if (k.keycode == 30 && k.pressed && k.keysym == 0x61) return true;
        return false;
    }));

    // Leaving the window, and clamping to the 1024x768 layout.
    host.server().inject_pointer_warp(5, 5);
    CHECK(a->wait_line("leave", 5000));
    host.server().inject_pointer_warp(5000, 5000);
    CHECK(host.wait([&] {
        auto p = host.server().cursor_position();
        return p.first >= 1023 && p.first < 1024 && p.second >= 767 && p.second < 768;
    }));
    host.server().inject_pointer_motion(-10000, 0);
    CHECK(host.wait([&] { return host.server().cursor_position().first == 0; }));
    host.server().warp_cursor(100, 200);
    CHECK(host.wait([&] { return host.server().cursor_position() == std::make_pair(100.0, 200.0); }));

    // Focus moves to a second window, and keys follow it.
    auto b = Child::spawn(client_args("in-b"), host.client_env());
    REQUIRE(b);
    REQUIRE(host.wait([&] { return host.window_by_app_id("in-b") != kNoWindow; }));
    WindowId wb = host.window_by_app_id("in-b");
    CHECK(host.wait([&] { return host.wm_focused() == wb; }));
    CHECK(b->wait_line("kbenter", 5000));
    CHECK(a->wait_line("kbleave", 5000));
    type_keys(host, {48});  // KEY_B
    CHECK(b->wait_line("key 48 1", 5000));
    host.wm_do([&](WindowManager& wm) { return wm.focus(wa); });
    CHECK(host.wait([&] { return host.wm_focused() == wa; }));
    CHECK(b->wait_line("kbleave", 5000));
    type_keys(host, {46});  // KEY_C
    CHECK(a->wait_line("key 46 1", 5000));
    CHECK(b->count("key 46") == 0);
}

// foot: type `echo hi>FILE` + Enter into the shell and check the file.
void foot(Host& host, const std::string& dir) {
    if (which("foot").empty()) {
        std::printf("SKIP foot: not installed\n");
        return;
    }
    std::string file = dir + "/typed.txt";
    auto env = host.client_env();
    env.push_back("SHELL=/bin/sh");
    env.push_back("ENV=");
    env.push_back("PS1=$ ");
    auto t = Child::spawn({"foot", "--app-id", "bc-foot", "-o", "main.shell=/bin/sh", "/bin/sh"}, env, true);
    REQUIRE(t);
    REQUIRE(host.wait([&] { return host.window_by_app_id("bc-foot") != kNoWindow; }, 10000));
    WindowId w = host.window_by_app_id("bc-foot");
    REQUIRE(host.wait([&] { return host.wm_focused() == w; }));
    // The terminal shows content before typing (its first frame arrived).
    auto snap = host.server().query(w);
    REQUIRE(snap);
    SurfaceId root = SurfaceId(snap->native);
    CHECK(host.wait([&] { return host.surface_pixel(root, 2, 2).has_value(); }, 10000));
    usleep(300 * 1000);  // let the shell start reading

    // evdev keycodes: e c h o SPACE h i SHIFT+. (">") ... ENTER
    const uint32_t kShift = 42;
    std::vector<uint32_t> echo = {18, 46, 35, 24, 57, 35, 23};
    type_keys(host, echo);
    host.server().inject_key(kShift, true);
    type_keys(host, {52});  // '.' with shift = '>'
    host.server().inject_key(kShift, false);
    // The file path: letters, digits, '/', '-', '.' and '_' only.
    static const std::map<char, uint32_t> keys = {
        {'a', 30}, {'b', 48}, {'c', 46}, {'d', 32}, {'e', 18}, {'f', 33}, {'g', 34}, {'h', 35}, {'i', 23},
        {'j', 36}, {'k', 37}, {'l', 38}, {'m', 50}, {'n', 49}, {'o', 24}, {'p', 25}, {'q', 16}, {'r', 19},
        {'s', 31}, {'t', 20}, {'u', 22}, {'v', 47}, {'w', 17}, {'x', 45}, {'y', 21}, {'z', 44}, {'0', 11},
        {'1', 2},  {'2', 3},  {'3', 4},  {'4', 5},  {'5', 6},  {'6', 7},  {'7', 8},  {'8', 9},  {'9', 10},
        {'/', 53}, {'-', 12}, {'.', 52}};
    for (char c : file) {
        bool upper = c >= 'A' && c <= 'Z';
        if (c == '_' || upper) {
            host.server().inject_key(kShift, true);
            type_keys(host, {c == '_' ? 12u : keys.at(char(c - 'A' + 'a'))});
            host.server().inject_key(kShift, false);
            continue;
        }
        auto it = keys.find(c);
        REQUIRE(it != keys.end());
        type_keys(host, {it->second});
    }
    type_keys(host, {28});  // ENTER
    std::string got;
    CHECK(host.wait(
        [&] {
            std::ifstream in(file);
            std::getline(in, got);
            return got == "hi";
        },
        10000));
    CHECK_EQ(got, std::string("hi"));
    if (got != "hi") std::fprintf(stderr, "foot output:\n%s\n", t->output().c_str());
    host.server().execute(Command{CloseWindow{w}});
    CHECK(t->wait_exit(5000));
}

}  // namespace

int main() {
    std::string dir = private_runtime_dir();
    if (dir.empty()) return 1;
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
    scripted(host);
    foot(host, dir);
    host.stop();
    return finish("test_wl_input");
}
