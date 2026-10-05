// Third-party clients against the server role on the headless backend:
// weston-simple-shm (animated, frame-callback paced), weston-simple-damage,
// gtk3-widget-factory (GTK3, client-side decorations) and a Qt 6 widget app
// (qtwayland). Each must map as a window with its title and pid, show content
// in its surface and in the host composite, keep committing when animated,
// and exit on CloseWindow. Missing programs are skipped, not failed.
#include "linux/wl_harness.h"
#include "printers.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <thread>

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

struct App {
    std::string label;
    std::vector<std::string> argv;
    std::vector<std::string> env;
    bool animated = false;
    // Run under dbus-run-session: GTK blocks on the desktop portal over a
    // user session bus that has a portal service it cannot reach (headless
    // boxes); a private bus has none. The window's pid is then the wrapper's
    // child.
    bool private_bus = false;
    int map_timeout_ms = 10000;
};

uint32_t parent_pid(uint32_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t p = s.rfind(')');
    if (p == std::string::npos) return 0;
    char state = 0;
    unsigned ppid = 0;
    std::sscanf(s.c_str() + p + 1, " %c %u", &state, &ppid);
    return ppid;
}

size_t commits_of(Host& host, SurfaceId s) {
    size_t n = 0;
    for (auto& c : host.server_events_of<SurfaceCommitted>())
        if (c.surface == s && c.new_buffer) ++n;
    return n;
}

void run_app(Host& host, MonitorId mon, const App& app) {
    std::string exe = app.argv[0][0] == '/' ? app.argv[0] : which(app.argv[0]);
    if (exe.empty() || access(exe.c_str(), X_OK) != 0) {
        std::printf("SKIP %s: not installed\n", app.label.c_str());
        return;
    }
    std::printf("-- %s\n", app.label.c_str());
    auto known = host.server().windows();
    auto env = host.client_env();
    env.insert(env.end(), app.env.begin(), app.env.end());
    std::vector<std::string> argv = app.argv;
    bool wrapped = app.private_bus && !which("dbus-run-session").empty();
    if (wrapped) argv.insert(argv.begin(), {"dbus-run-session", "--"});
    auto c = Child::spawn(argv, env, true);
    REQUIRE(c);
    WindowId w = kNoWindow;
    bool mapped = host.wait(
        [&] {
            for (WindowId id : host.server().windows())
                if (std::find(known.begin(), known.end(), id) == known.end()) w = id;
            return w != kNoWindow;
        },
        app.map_timeout_ms);
    if (!mapped) std::fprintf(stderr, "%s output:\n%s\n", app.label.c_str(), c->output().c_str());
    REQUIRE(mapped);
    auto snap = host.server().query(w);
    REQUIRE(snap);
    std::printf("   title '%s' app_id '%s' frame %d,%d %dx%d\n", snap->title.c_str(), snap->app_id.c_str(),
                snap->frame.x, snap->frame.y, snap->frame.width, snap->frame.height);
    CHECK(!snap->title.empty());
    if (wrapped)
        CHECK(parent_pid(snap->process_id) == uint32_t(c->pid()));
    else
        CHECK(snap->process_id == uint32_t(c->pid()));
    CHECK(snap->frame.width > 0 && snap->frame.height > 0);
    CHECK(host.wait([&] { return host.wm_focused() == w; }));

    // Content: a frame in the root surface, and drawn into the output.
    SurfaceId root = SurfaceId(snap->native);
    CHECK(host.wait([&] { return host.surface_pixel(root, 0, 0).has_value(); }));
    CHECK(host.wait([&] { return host.last_drawn(mon).count(root) == 1; }));
    snap = host.server().query(w);
    int32_t cx = snap->frame.x + snap->frame.width / 2, cy = snap->frame.y + snap->frame.height / 2;
    CHECK(host.wait([&] {
        auto p = host.output_pixel(mon, uint32_t(std::max(0, cx)), uint32_t(std::max(0, cy)));
        return p && *p != 0xFF203040u;
    }));

    if (app.animated) {
        size_t before = commits_of(host, root);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        size_t n = commits_of(host, root) - before;
        std::printf("   %zu commits in 0.5 s\n", n);
        // Paced by frame callbacks at the output's 60 Hz: never unthrottled
        // (40), and still animating. The floor is half the rate, except in an
        // unoptimized build (Debug, coverage), where the test host's CPU
        // compositor can fall well below 60 Hz and the frames only have to
        // keep coming.
#if defined(__OPTIMIZE__)
        const size_t min_commits = 15;
#else
        const size_t min_commits = 5;
#endif
        CHECK(n >= min_commits && n <= 40);
    }

    CHECK(host.server().execute(Command{CloseWindow{w}}));
    int status = -1;
    bool exited = c->wait_exit(5000, &status);
    if (!exited) std::fprintf(stderr, "%s did not exit on close:\n%s\n", app.label.c_str(), c->output().c_str());
    CHECK(exited);
    CHECK(host.wait([&] {
        auto ws = host.server().windows();
        return std::find(ws.begin(), ws.end(), w) == ws.end();
    }));
}

void run(Host& host) {
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    MonitorId mon = host.wm_monitors()[0].id;
    const std::string qt = "/usr/lib/x86_64-linux-gnu/qt6/examples/widgets/widgets/calculator/bin/calculator";
    std::vector<App> apps = {
        {"weston-simple-shm", {"weston-simple-shm"}, {}, true},
        {"weston-simple-damage", {"weston-simple-damage"}, {}, true},
        {"gtk3-widget-factory", {"gtk3-widget-factory"}, {"NO_AT_BRIDGE=1", "GSETTINGS_BACKEND=memory"}, false, true},
        {"qt6 calculator", {qt}, {"QT_QPA_PLATFORM=wayland", "QT_WAYLAND_DISABLE_WINDOWDECORATION=1"}},
    };
    for (auto& a : apps) run_app(host, mon, a);
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.initial_output_size = Size{1280, 900};
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    run(host);
    host.stop();
    return finish("test_wl_clients");
}
