// X11 selections bridged to the Wayland clipboard and primary selection
// (XWayland's window manager): wl-copy -> xclip / xsel, and xclip / xsel ->
// wl-paste, for both CLIPBOARD and PRIMARY, with an X11 window focused as
// in a real session.
#include "linux/wl_harness.h"
#include "printers.h"

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

// Runs a command to completion; its stdout (lines joined) or nullopt.
std::optional<std::string> run_output(Host& host, const std::vector<std::string>& argv, int timeout_ms = 5000) {
    auto p = Child::spawn(argv, host.client_env());
    if (!p) return std::nullopt;
    int status = -1;
    if (!p->wait_exit(timeout_ms, &status) || status != 0) return std::nullopt;
    std::string s = p->output();
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

// Polls a reader until it returns `want` (the bridge is asynchronous).
bool eventually(Host& host, const std::vector<std::string>& argv, const std::string& want, std::string* got) {
    return host.wait(
        [&] {
            *got = run_output(host, argv).value_or("<none>");
            return *got == want;
        },
        10000);
}

void run(Host& host) {
    for (const char* tool : {"xclip", "xsel", "wl-copy", "wl-paste"})
        if (which(tool).empty()) {
            std::printf("SKIP: %s not installed\n", tool);
            return;
        }
#ifdef BC_X11_CLIENT
    // An X11 window holds the focus.
    auto xwin = Child::spawn({BC_X11_CLIENT, "--class", "BcSel"}, host.client_env());
    REQUIRE(xwin);
    // In stages, so a failure says which one: the X window maps, the host's
    // WM focuses it, and the X client sees the focus.
    WindowId w = kNoWindow;
    bool managed = host.wait(
        [&] {
            for (WindowId id : host.server().windows())
                if (auto s = host.server().query(id); s && s->app_id == "BcSel") w = id;
            return w != kNoWindow;
        },
        10000);
    bool focused = managed && host.wait([&] { return host.wm_focused() == w; });
    bool seen = focused && xwin->wait_line("focus_in", 10000);
    if (!seen) {
        std::fprintf(stderr, "X window managed %d, WM-focused %d (wm_focused %llu, window %llu); client output:\n%s\n",
                     managed, focused, (unsigned long long)host.wm_focused(), (unsigned long long)w,
                     xwin->output().c_str());
    }
    REQUIRE(seen);
#endif
    std::string got;

    // Wayland -> X11, clipboard and primary.
    auto c1 = Child::spawn({"wl-copy", "--foreground", "from wayland"}, host.client_env());
    REQUIRE(c1);
    CHECK(eventually(host, {"xclip", "-o", "-selection", "clipboard"}, "from wayland", &got));
    CHECK_EQ(got, std::string("from wayland"));
    auto c2 = Child::spawn({"wl-copy", "--foreground", "--primary", "wayland primary"}, host.client_env());
    REQUIRE(c2);
    CHECK(eventually(host, {"xsel", "--output", "--primary"}, "wayland primary", &got));
    CHECK_EQ(got, std::string("wayland primary"));
    CHECK(eventually(host, {"xsel", "--output", "--clipboard"}, "from wayland", &got));

    // X11 -> Wayland: xclip owns CLIPBOARD, xsel owns PRIMARY.
    auto x1 = Child::spawn({"xclip", "-selection", "clipboard", "-i", "-quiet"}, host.client_env());
    REQUIRE(x1);
    x1->send("from x11");
    x1->close_stdin();
    CHECK(eventually(host, {"wl-paste", "-n"}, "from x11", &got));
    CHECK_EQ(got, std::string("from x11"));
    CHECK(c1->wait_exit(5000));  // its wl-copy source was replaced
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<SelectionChanged>();
        for (auto it = ev.rbegin(); it != ev.rend(); ++it)
            if (!it->primary) return !it->mime_types.empty();
        return false;
    }));
    auto x2 = Child::spawn({"xsel", "--input", "--primary", "--nodetach"}, host.client_env());
    REQUIRE(x2);
    x2->send("x11 primary");
    x2->close_stdin();
    CHECK(eventually(host, {"wl-paste", "-n", "--primary"}, "x11 primary", &got));
    CHECK_EQ(got, std::string("x11 primary"));
    // And a Wayland client reads what X11 owns.
    CHECK(eventually(host, {"wl-paste", "-n"}, "from x11", &got));

    x1->kill_now();
    x2->kill_now();
    c2->kill_now();
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    setenv("XWAYLAND_NO_GLAMOR", "1", 1);  // see test_wl_xwayland
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.xwayland = XwaylandMode::Lazy;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    if (host.server().xwayland_display().empty() || which("Xwayland").empty()) {
        std::printf("SKIP: wlroots was built without XWayland, or Xwayland is not installed\n");
        return 77;
    }
    run(host);
    host.stop();
    return finish("test_wl_xselection");
}
