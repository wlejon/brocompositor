// XWayland through the server role: X11 clients are windows of the same
// model (app_id = WM_CLASS class, title, _NET_WM_PID, transient-for owner),
// override-redirect windows are unmanaged surfaces, focus / keys / pointer
// reach them, WM_DELETE_WINDOW closes them, _NET_WM_STATE requests become
// WindowRequests, and oversized windows are placed inside the work area.
// Real clients: xterm (typing into a shell), xeyes, gtk3-widget-factory
// under GDK_BACKEND=x11, plus the plain-xcb bc_x11_client for exact checks.
#include "linux/wl_harness.h"
#include "printers.h"

#include <fstream>

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

constexpr uint32_t kBtnLeft = 272;

// The newest window whose snapshot satisfies `pred`.
WindowId find_window(Host& host, const std::function<bool(const WindowSnapshot&)>& pred) {
    WindowId found = kNoWindow;
    for (WindowId id : host.server().windows())
        if (auto s = host.server().query(id); s && pred(*s)) found = std::max(found, id);
    return found;
}

WindowId wait_window(Host& host, const std::function<bool(const WindowSnapshot&)>& pred, int ms = 10000) {
    WindowId id = kNoWindow;
    if (host.wait([&] { return (id = find_window(host, pred)) != kNoWindow; }, ms)) return id;
    for (WindowId w : host.server().windows())
        if (auto s = host.server().query(w))
            std::fprintf(stderr, "  window %llu app_id '%s' title '%s' pid %u\n", (unsigned long long)w,
                         s->app_id.c_str(), s->title.c_str(), s->process_id);
    return kNoWindow;
}

std::optional<uint32_t> fresh_pixel(Host& host, int x, int y) {
    MonitorId out = host.server().outputs().front().id;
    uint64_t base = host.presents(out);
    host.server().schedule_frame(out);
    host.wait([&] { return host.presents(out) >= base + 2; });
    return host.output_pixel(out, uint32_t(x), uint32_t(y));
}

#ifdef BC_X11_CLIENT
void scripted(Host& host) {
    std::printf("-- bc_x11_client\n");
    auto x = Child::spawn({BC_X11_CLIENT, "--class", "BcX11", "--instance", "bcx11", "--title", "x title", "--size",
                           "300x200", "--color", "C03080"},
                          host.client_env());
    REQUIRE(x);
    WindowId w = wait_window(host, [](const WindowSnapshot& s) { return s.app_id == "BcX11"; });
    REQUIRE(w != kNoWindow);
    REQUIRE(x->wait_line("mapped", 5000));
    auto s = host.server().query(w);
    REQUIRE(s);
    CHECK_EQ(s->title, std::string("x title"));
    CHECK_EQ(s->class_name, std::string("bcx11"));
    CHECK_EQ(s->process_id, uint32_t(x->pid()));
    CHECK_EQ(s->frame.width, 300);
    CHECK_EQ(s->frame.height, 200);
    CHECK(host.wait([&] {
        for (auto& st : host.server_events_of<XwaylandStatus>())
            if (st.running) return true;
        return false;
    }));

    // Focus and keys (X keycode = evdev + 8).
    CHECK(host.wait([&] { return host.wm_focused() == w; }));
    CHECK(x->wait_line("focus_in", 5000));
    tap_key(host.server(), kKeyA);
    CHECK(x->wait_line("key 38 1", 5000));
    // Its pixels and the pointer.
    s = host.server().query(w);
    int cx = s->frame.x + 150, cy = s->frame.y + 100;
    CHECK(host.wait([&] { return fresh_pixel(host, cx, cy) == 0xFFC03080u; }));
    host.server().inject_pointer_warp(cx, cy);
    host.server().inject_pointer_button(kBtnLeft, true);
    host.server().inject_pointer_button(kBtnLeft, false);
    CHECK(x->wait_line("button 1 1", 5000));

    // _NET_WM_STATE requests reach the host, whose answer reaches the client.
    x->send("fullscreen\n");
    CHECK(host.wait([&] {
        for (auto& r : host.server_events_of<WindowRequest>())
            if (r.window == w && r.kind == WindowRequestKind::Fullscreen) return true;
        return false;
    }));
    CHECK(x->wait_line("state fullscreen", 5000));
    CHECK(host.wait([&] { return host.server().query(w)->fullscreen; }));
    x->send("unfullscreen\n");
    CHECK(host.wait([&] { return !host.server().query(w)->fullscreen; }));
    x->send("maximize\n");
    CHECK(x->wait_line("state maximized_vert maximized_horz", 5000));
    CHECK(host.wait([&] { return host.server().query(w)->maximized; }));

    // A transient dialog names its owner; an override-redirect window is an
    // unmanaged surface above everything, owned by the same client's window.
    auto d = Child::spawn({BC_X11_CLIENT, "--class", "BcDlg", "--title", "main", "--transient", "--override",
                           "700,500,80x60"},
                          host.client_env());
    REQUIRE(d);
    WindowId dm = wait_window(host, [](const WindowSnapshot& s) { return s.app_id == "BcDlg" && s.title == "main"; });
    WindowId dd = wait_window(host, [](const WindowSnapshot& s) { return s.title == "main dialog"; });
    REQUIRE(dm != kNoWindow && dd != kNoWindow);
    CHECK_EQ(host.server().query(dd)->owner, dm);
    CHECK(host.wait([&] { return !host.server().unmanaged_surfaces().empty(); }));
    auto um = host.server().unmanaged_surfaces();
    REQUIRE(!um.empty());
    CHECK_EQ(um.back().rect, (Rect{700, 500, 80, 60}));
    CHECK_EQ(um.back().process_id, uint32_t(d->pid()));
    CHECK(!host.server_events_of<UnmanagedSurfaceAdded>().empty());
    CHECK(host.wait([&] { return fresh_pixel(host, 720, 520) == 0xFFE0E000u; }));

    // WM_DELETE_WINDOW.
    host.server().close(w);
    CHECK(x->wait_line("delete", 5000));
    CHECK(x->wait_exit(5000));
    CHECK(host.wait([&] { return !host.server().query(w); }));
    d->send("quit\n");
    CHECK(d->wait_exit(5000));
    CHECK(host.wait([&] { return host.server().unmanaged_surfaces().empty(); }));
    CHECK(!host.server_events_of<UnmanagedSurfaceRemoved>().empty());

    // A window larger than the output is shrunk into the work area and
    // centred, not left at x = 0 at full size.
    auto big = Child::spawn({BC_X11_CLIENT, "--class", "BcBig", "--size", "3000x2000"}, host.client_env());
    REQUIRE(big);
    WindowId wb = wait_window(host, [](const WindowSnapshot& s) { return s.app_id == "BcBig"; });
    REQUIRE(wb != kNoWindow);
    Rect area = host.server().outputs().front().work_area;
    CHECK(host.wait([&] {
        Rect f = host.server().query(wb)->frame;
        return f.width <= area.width && f.height <= area.height && f.x >= area.x && f.y >= area.y &&
               f.x + f.width <= area.x + area.width && f.y + f.height <= area.y + area.height;
    }));
    std::printf("big window frame %d,%d %dx%d\n", host.server().query(wb)->frame.x, host.server().query(wb)->frame.y,
                host.server().query(wb)->frame.width, host.server().query(wb)->frame.height);
    big->send("quit\n");
    CHECK(big->wait_exit(5000));
}
#endif

void xterm(Host& host, const std::string& dir) {
    if (which("xterm").empty()) return (void)std::printf("SKIP xterm: not installed\n");
    std::printf("-- xterm\n");
    std::string file = dir + "/xterm.txt";
    auto t = Child::spawn({"xterm", "-class", "BcXTerm", "-e", "sh", "-c", "read l; echo \"$l\" > " + file},
                          host.client_env(), true);
    REQUIRE(t);
    WindowId w = wait_window(host, [](const WindowSnapshot& s) { return s.app_id == "BcXTerm"; });
    REQUIRE(w != kNoWindow);
    CHECK_EQ(host.server().query(w)->process_id, uint32_t(t->pid()));
    CHECK(host.wait([&] { return host.wm_focused() == w; }));
    SurfaceId root = SurfaceId(host.server().query(w)->native);
    CHECK(host.wait([&] { return host.surface_pixel(root, 2, 2).has_value(); }, 10000));
    usleep(300 * 1000);
    type_text(host.server(), "hello x11\n");
    std::string got;
    CHECK(host.wait(
        [&] {
            std::ifstream in(file);
            std::getline(in, got);
            return got == "hello x11";
        },
        10000));
    CHECK_EQ(got, std::string("hello x11"));
    CHECK(t->wait_exit(5000));
}

void xeyes(Host& host) {
    if (which("xeyes").empty()) return (void)std::printf("SKIP xeyes: not installed\n");
    std::printf("-- xeyes\n");
    auto t = Child::spawn({"xeyes"}, host.client_env(), true);
    REQUIRE(t);
    WindowId w = wait_window(host, [](const WindowSnapshot& s) { return s.app_id == "XEyes"; });
    REQUIRE(w != kNoWindow);
    SurfaceId root = SurfaceId(host.server().query(w)->native);
    CHECK(host.wait([&] { return host.surface_pixel(root, 5, 5).has_value(); }, 10000));
    host.server().close(w);  // WM_DELETE_WINDOW: xeyes exits
    CHECK(t->wait_exit(5000));
}

void gtk_x11(Host& host) {
    if (which("gtk3-widget-factory").empty()) return (void)std::printf("SKIP gtk3-widget-factory: not installed\n");
    std::printf("-- gtk3-widget-factory (GDK_BACKEND=x11)\n");
    auto env = host.client_env();
    env.push_back("GDK_BACKEND=x11");
    env.push_back("NO_AT_BRIDGE=1");
    env.push_back("GTK_A11Y=none");
    // A GApplication waits for a session bus before it shows its window.
    std::vector<std::string> argv = {"gtk3-widget-factory"};
    if (!which("dbus-run-session").empty()) argv.insert(argv.begin(), "dbus-run-session");
    auto t = Child::spawn(argv, env, true);
    REQUIRE(t);
    WindowId w = wait_window(host, [&](const WindowSnapshot& s) { return s.app_id == "Gtk3-widget-factory"; }, 30000);
    if (w == kNoWindow) std::fprintf(stderr, "gtk3-widget-factory output:\n%s\n", t->output().c_str());
    REQUIRE(w != kNoWindow);
    auto s = host.server().query(w);
    std::printf("gtk x11 window: title '%s' frame %d,%d %dx%d\n", s->title.c_str(), s->frame.x, s->frame.y,
                s->frame.width, s->frame.height);
    CHECK(s->process_id != 0);
    // Its minimum size (about 1350x750) is wider than the 1024x768 output:
    // it cannot be shrunk to fit, so it starts at the work area's left edge.
    Rect area = host.server().outputs().front().work_area;
    CHECK(s->frame.y >= area.y && s->frame.y + s->frame.height <= area.y + area.height);
    CHECK(s->frame.width <= area.width ? s->frame.x + s->frame.width <= area.x + area.width : s->frame.x == area.x);
    SurfaceId root = SurfaceId(s->native);
    CHECK(host.wait([&] { return host.surface_pixel(root, 10, 10).has_value(); }, 20000));
    host.server().close(w);
    if (!t->wait_exit(10000)) t->kill_now();
}

}  // namespace

int main() {
    std::string dir = private_runtime_dir();
    if (dir.empty()) return 1;
    // The test host composites on the CPU and advertises LINEAR dmabufs only;
    // glamor on some drivers (NVIDIA) cannot render into those, so Xwayland
    // (which inherits this environment) uses shm buffers.
    setenv("XWAYLAND_NO_GLAMOR", "1", 1);
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{1024, 768};
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
    CHECK(!host.server_events_of<XwaylandStatus>().empty() ||
          host.wait([&] { return !host.server_events_of<XwaylandStatus>().empty(); }));
#ifdef BC_X11_CLIENT
    scripted(host);
#else
    std::printf("SKIP bc_x11_client: built without xcb\n");
#endif
    xterm(host, dir);
    xeyes(host);
    gtk_x11(host);
    host.stop();
    return finish("test_wl_xwayland");
}
