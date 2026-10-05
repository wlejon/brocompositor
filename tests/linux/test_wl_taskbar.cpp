// Taskbars and launchers as clients: ext-foreign-toplevel-list announces
// windows and their closing; wlrctl (wlr-foreign-toplevel-management) lists,
// focuses, minimizes and closes them through host-decided WindowRequests
// (foreign = true); xdg-activation turns a token into an Activate request.
#include "linux/wl_harness.h"
#include "printers.h"

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

std::unique_ptr<Child> window(Host& host, const std::string& app_id, const std::string& title) {
    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", app_id, "--title", title, "--size", "200x150"}, host.client_env());
    if (!c || !c->wait_line("ready", 5000)) return nullptr;
    if (!host.wait([&] { return host.window_by_app_id(app_id) != kNoWindow; })) return nullptr;
    return c;
}

bool saw_request(Host& host, WindowId w, WindowRequestKind kind, bool foreign) {
    return host.wait([&] {
        for (auto& r : host.server_events_of<WindowRequest>())
            if (r.window == w && r.kind == kind && r.foreign == foreign) return true;
        return false;
    });
}

bool wlrctl(Host& host, std::vector<std::string> args, std::string* out = nullptr) {
    args.insert(args.begin(), "wlrctl");
    auto p = Child::spawn(args, host.client_env(), !out);
    if (!p) return false;
    int status = -1;
    if (!p->wait_exit(5000, &status)) return false;
    if (out) *out = p->output();
    return status == 0;
}

void list(Host& host) {
    std::printf("-- ext-foreign-toplevel-list\n");
    auto bar = Child::spawn({BC_WL_SESSION_CLIENT, "--toplevels"}, host.client_env());
    REQUIRE(bar);
    REQUIRE(bar->wait_line("list_done", 5000));
    auto a = window(host, "alpha", "Alpha");
    REQUIRE(a);
    CHECK(bar->wait_line("toplevel alpha Alpha", 5000));
    auto b = window(host, "beta", "Beta");
    REQUIRE(b);
    CHECK(bar->wait_line("toplevel beta Beta", 5000));
    b->send("quit\n");
    CHECK(b->wait_exit(5000));
    CHECK(bar->wait_line("toplevel_closed beta", 5000));
    a->send("quit\n");
    CHECK(bar->wait_line("toplevel_closed alpha", 5000));
}

void manage(Host& host) {
    if (which("wlrctl").empty()) return (void)std::printf("SKIP wlrctl: not installed\n");
    std::printf("-- wlrctl toplevel (foreign-toplevel-management)\n");
    auto a = window(host, "gamma", "Gamma");
    REQUIRE(a);
    auto b = window(host, "delta", "Delta");
    REQUIRE(b);
    WindowId ga = host.window_by_app_id("gamma"), dl = host.window_by_app_id("delta");
    CHECK(host.wait([&] { return host.wm_focused() == dl; }));

    std::string listing;
    CHECK(wlrctl(host, {"toplevel", "list"}, &listing));
    CHECK(listing.find("gamma") != std::string::npos);
    CHECK(listing.find("delta") != std::string::npos);
    CHECK(wlrctl(host, {"toplevel", "find", "app_id:gamma"}));
    CHECK(!wlrctl(host, {"toplevel", "find", "app_id:nonesuch"}));

    // Focus: an Activate request the host honours.
    CHECK(wlrctl(host, {"toplevel", "focus", "app_id:gamma"}));
    CHECK(saw_request(host, ga, WindowRequestKind::Activate, true));
    CHECK(host.wait([&] { return host.wm_focused() == ga; }));
    CHECK(a->wait_line("kbenter", 5000));

    // Minimize, then focus brings it back.
    CHECK(wlrctl(host, {"toplevel", "minimize", "app_id:delta"}));
    CHECK(saw_request(host, dl, WindowRequestKind::Minimize, true));
    CHECK(host.wait([&] {
        auto s = host.server().query(dl);
        return s && s->minimized;
    }));
    CHECK(wlrctl(host, {"toplevel", "focus", "app_id:delta"}));
    CHECK(host.wait([&] {
        auto s = host.server().query(dl);
        return s && !s->minimized && host.wm_focused() == dl;
    }));

    // Close: a Close request; the host closes, the client exits.
    CHECK(wlrctl(host, {"toplevel", "close", "app_id:gamma"}));
    CHECK(saw_request(host, ga, WindowRequestKind::Close, true));
    CHECK(a->wait_line("close", 5000));
    CHECK(a->wait_exit(5000));
    CHECK(host.wait([&] { return host.server().query(ga) == std::nullopt; }));
    b->send("quit\n");
    b->wait_exit(5000);
}

void activation(Host& host) {
    std::printf("-- xdg-activation\n");
    auto c = Child::spawn({BC_WL_INPUT_CLIENT, "--app-id", "activator", "--activate-self"}, host.client_env());
    REQUIRE(c);
    CHECK(c->wait_line("token ", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("activator") != kNoWindow; }));
    WindowId w = host.window_by_app_id("activator");
    CHECK(saw_request(host, w, WindowRequestKind::Activate, false));
    CHECK(host.wait([&] { return host.wm_focused() == w; }));
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
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
    list(host);
    manage(host);
    activation(host);
    host.stop();
    return finish("test_wl_taskbar");
}
