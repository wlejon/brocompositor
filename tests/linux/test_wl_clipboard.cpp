// Clipboard through the server role: wl-clipboard (data-control) sets and
// reads the regular and primary selections, the host sees SelectionChanged
// with the offered MIME types, a replaced source is cancelled (its wl-copy
// exits), and clearing empties the selection. A client's drag shows its
// icon to the host (drag_icon), at its offset against the pointer.
#include "linux/wl_harness.h"
#include "printers.h"

#include <algorithm>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

// Runs wl-paste with `args`; returns its output (lines joined) or nullopt.
std::optional<std::string> paste(Host& host, std::vector<std::string> args) {
    args.insert(args.begin(), "wl-paste");
    auto p = Child::spawn(args, host.client_env());
    if (!p) return std::nullopt;
    int status = -1;
    if (!p->wait_exit(5000, &status)) return std::nullopt;
    if (status != 0) return std::nullopt;
    std::string s = p->output();  // lines, each '\n'-terminated by Child
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

bool has_selection_event(Host& host, bool primary, const std::string& mime) {
    for (auto& e : host.server_events_of<SelectionChanged>())
        if (e.primary == primary && std::count(e.mime_types.begin(), e.mime_types.end(), mime)) return true;
    return false;
}

void drag_icon(Host& host);

void run(Host& host) {
    if (which("wl-copy").empty() || which("wl-paste").empty()) {
        std::printf("SKIP: wl-clipboard not installed\n");
        drag_icon(host);
        return;
    }
    // Regular selection round trip.
    auto c1 = Child::spawn({"wl-copy", "--foreground", "hello from bc"}, host.client_env());
    REQUIRE(c1);
    CHECK(host.wait([&] { return has_selection_event(host, false, "text/plain"); }));
    CHECK_EQ(paste(host, {"-n"}).value_or("<none>"), std::string("hello from bc"));

    // A second source with a custom type replaces (cancels) the first.
    auto c2 = Child::spawn({"wl-copy", "--foreground", "-t", "text/x-bc", "custom data"}, host.client_env());
    REQUIRE(c2);
    CHECK(host.wait([&] { return has_selection_event(host, false, "text/x-bc"); }));
    int status = -1;
    CHECK(c1->wait_exit(5000, &status));
    auto types = paste(host, {"--list-types"});
    CHECK(types && types->find("text/x-bc") != std::string::npos);
    CHECK_EQ(paste(host, {"-n", "-t", "text/x-bc"}).value_or("<none>"), std::string("custom data"));

    // Primary selection, independent of the regular one.
    auto c3 = Child::spawn({"wl-copy", "--foreground", "--primary", "primary text"}, host.client_env());
    REQUIRE(c3);
    CHECK(host.wait([&] { return has_selection_event(host, true, "text/plain"); }));
    CHECK_EQ(paste(host, {"-n", "--primary"}).value_or("<none>"), std::string("primary text"));
    CHECK_EQ(paste(host, {"-n", "-t", "text/x-bc"}).value_or("<none>"), std::string("custom data"));

    // Clearing: the source is cancelled and nothing is offered.
    auto clear = Child::spawn({"wl-copy", "--clear"}, host.client_env());
    REQUIRE(clear);
    CHECK(clear->wait_exit(5000));
    CHECK(c2->wait_exit(5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<SelectionChanged>();
        return !ev.empty() && !ev.back().primary && ev.back().mime_types.empty();
    }));
    CHECK(!paste(host, {"-n"}).has_value());
    c3->kill_now();

    // wl_data_device: a focused client sets the selection (with its keyboard
    // enter serial); wl-paste reads it through data-control.
    auto copier = Child::spawn({BC_WL_CLIENT, "--app-id", "copier", "--copy", "from data device"}, host.client_env());
    REQUIRE(copier);
    CHECK(copier->wait_line("copied", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<SelectionChanged>();
        return !ev.empty() && !ev.back().primary &&
               std::count(ev.back().mime_types.begin(), ev.back().mime_types.end(), "text/plain;charset=utf-8");
    }));
    CHECK_EQ(paste(host, {"-n"}).value_or("<none>"), std::string("from data device"));

    // A focused data-device client receives the selection on focus, and a
    // data-control source (wl-copy) replacing it reaches it too.
    auto paster = Child::spawn({BC_WL_CLIENT, "--app-id", "paster", "--paste"}, host.client_env());
    REQUIRE(paster);
    CHECK(paster->wait_line("kbenter", 5000));
    CHECK(paster->wait_line("paste from data device", 5000));
    auto c4 = Child::spawn({"wl-copy", "--foreground", "to data device"}, host.client_env());
    REQUIRE(c4);
    CHECK(paster->wait_line("paste to data device", 5000));
    CHECK(copier->wait_line("cancelled", 5000));
    c4->kill_now();
    copier->kill_now();
    paster->kill_now();

    drag_icon(host);
}

// A drag a client starts (wl_data_device.start_drag) with an icon: the host
// hears DragIconChanged and drag_icon() gives the icon's surface, its offset
// against the pointer (the client's wl_surface.offset) and size, to draw at
// the pointer; the release over no taker cancels the drag, and the icon goes.
void drag_icon(Host& host) {
    constexpr uint32_t kBtnLeft = 0x110;
    auto d = Child::spawn({BC_WL_CLIENT, "--app-id", "dragger", "--drag", "dragged text"}, host.client_env());
    REQUIRE(d);
    REQUIRE(d->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("dragger") != kNoWindow; }));
    const WindowId w = host.window_by_app_id("dragger");
    Rect f = host.server().query(w)->frame;
    CHECK(!host.server().drag_icon().has_value());
    host.server().inject_pointer_warp(f.x + 20, f.y + 20);
    REQUIRE(d->wait_line("enter 20 20", 5000));
    host.server().inject_pointer_button(kBtnLeft, true);
    REQUIRE(d->wait_line("drag-started", 5000));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<DragIconChanged>();
        return !ev.empty() && ev.back().surface != kNoSurface;
    }));
    CHECK(host.wait([&] {
        auto icon = host.server().drag_icon();
        return icon && icon->size.width == 16 && icon->size.height == 8;
    }));
    if (auto icon = host.server().drag_icon()) {
        CHECK_EQ(icon->offset.x, -4);
        CHECK_EQ(icon->offset.y, -3);
        CHECK(host.server().surface(icon->surface) != nullptr);
    }
    host.server().inject_pointer_warp(f.x + 60, f.y + 50);
    host.server().inject_pointer_button(kBtnLeft, false);
    CHECK(d->wait_line("drag-cancelled", 5000));
    CHECK(host.wait([&] { return !host.server().drag_icon().has_value(); }));
    CHECK(host.wait([&] {
        auto ev = host.server_events_of<DragIconChanged>();
        return !ev.empty() && ev.back().surface == kNoSurface;
    }));
    d->kill_now();
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    run(host);
    host.stop();
    return finish("test_wl_clipboard");
}
