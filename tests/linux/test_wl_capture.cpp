// Screen capture from host-rendered outputs: grim (wlr-screencopy or
// ext-image-copy-capture, whichever it prefers) writes a PPM whose pixels
// are the host's composite; ext-image-copy-capture output sessions deliver
// a first frame at once and a second only after damage; a window source
// (foreign toplevel) is rendered by the host through CaptureRequest; and
// with host_capture_copies every output copy goes to the host as well.
#include "linux/wl_harness.h"
#include "printers.h"

#include <fstream>
#include <iterator>
#include <sstream>

#include <unistd.h>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

constexpr uint32_t kRed = 0xFFFF0000, kBackground = 0xFF203040;

struct Ppm {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    std::optional<uint32_t> pixel(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return std::nullopt;
        const uint8_t* p = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        return 0xFF000000u | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
    }
};

std::optional<Ppm> read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::istringstream hdr(data);
    std::string magic;
    Ppm p;
    int maxval = 0;
    hdr >> magic >> p.w >> p.h >> maxval;
    if (!hdr || magic != "P6" || maxval != 255 || p.w <= 0 || p.h <= 0) return std::nullopt;
    size_t off = size_t(hdr.tellg()) + 1;  // one whitespace byte after maxval
    size_t n = size_t(p.w) * size_t(p.h) * 3;
    if (data.size() < off + n) return std::nullopt;
    p.rgb.assign(data.begin() + std::ptrdiff_t(off), data.begin() + std::ptrdiff_t(off + n));
    return p;
}

std::unique_ptr<Child> window(Host& host, const std::string& app_id, const std::string& color,
                              const std::string& size = "300x200") {
    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", app_id, "--color", color, "--size", size}, host.client_env());
    if (!c || !c->wait_line("ready", 5000)) return nullptr;
    if (!host.wait([&] { return host.window_by_app_id(app_id) != kNoWindow; })) return nullptr;
    return c;
}

Rect frame_of(Host& host, const std::string& app_id) {
    auto s = host.server().query(host.window_by_app_id(app_id));
    return s ? s->frame : Rect{};
}

// The value of a "pixel <x> <y> <AARRGGBB>" line.
std::optional<uint32_t> pixel_line(const Child& c, int x, int y) {
    std::string prefix = "pixel " + std::to_string(x) + " " + std::to_string(y) + " ";
    for (const std::string& l : c.lines())
        if (l.rfind(prefix, 0) == 0) return uint32_t(std::strtoul(l.c_str() + prefix.size(), nullptr, 16));
    return std::nullopt;
}

void grim(Host& host, const std::string& dir) {
    if (which("grim").empty()) return (void)std::printf("SKIP grim: not installed\n");
    std::printf("-- grim\n");
    Rect f = frame_of(host, "red");
    // Let the host present the window first.
    MonitorId out = host.wm_monitors().at(0).id;
    CHECK(host.wait([&] { return host.output_pixel(out, uint32_t(f.x + 10), uint32_t(f.y + 10)) == kRed; }));

    std::string file = dir + "/shot.ppm";
    auto g = Child::spawn({"grim", "-t", "ppm", file}, host.client_env(), true);
    REQUIRE(g);
    int status = -1;
    CHECK(g->wait_exit(10000, &status));
    CHECK_EQ(status, 0);
    auto shot = read_ppm(file);
    REQUIRE(shot);
    Rect b = host.wm_monitors().at(0).bounds;
    CHECK_EQ(shot->w, b.width);
    CHECK_EQ(shot->h, b.height);
    CHECK_EQ(shot->pixel(f.x + 10, f.y + 10), std::optional<uint32_t>(kRed));
    CHECK_EQ(shot->pixel(f.x + f.width - 5, f.y + f.height - 5), std::optional<uint32_t>(kRed));
    CHECK_EQ(shot->pixel(b.width - 2, b.height - 2), std::optional<uint32_t>(kBackground));

    // A region (-g): the window's interior only.
    std::string geom = std::to_string(f.x + 20) + "," + std::to_string(f.y + 20) + " 40x30";
    std::string part = dir + "/part.ppm";
    auto r = Child::spawn({"grim", "-g", geom, "-t", "ppm", part}, host.client_env(), true);
    REQUIRE(r);
    CHECK(r->wait_exit(10000, &status));
    CHECK_EQ(status, 0);
    auto crop = read_ppm(part);
    REQUIRE(crop);
    CHECK_EQ(crop->w, 40);
    CHECK_EQ(crop->h, 30);
    CHECK_EQ(crop->pixel(0, 0), std::optional<uint32_t>(kRed));
    CHECK_EQ(crop->pixel(39, 29), std::optional<uint32_t>(kRed));
}

void output_session(Host& host) {
    std::printf("-- ext-image-copy-capture: output source, damage-paced frames\n");
    Rect f = frame_of(host, "red");
    std::string px = std::to_string(f.x + 10) + "," + std::to_string(f.y + 10);
    auto c = Child::spawn({BC_WL_SESSION_CLIENT, "--capture", "output", "--frames", "2", "--pixel", px, "--pixel", "2,2"},
                          host.client_env());
    REQUIRE(c);
    Rect b = host.wm_monitors().at(0).bounds;
    CHECK(c->wait_line("session " + std::to_string(b.width) + " " + std::to_string(b.height), 5000));
    REQUIRE(c->wait_line("frame_ready 1", 5000));
    CHECK_EQ(pixel_line(*c, f.x + 10, f.y + 10), std::optional<uint32_t>(kRed));
    CHECK_EQ(pixel_line(*c, 2, 2), std::optional<uint32_t>(kBackground));
    // Nothing changes on screen: the second frame waits.
    usleep(400 * 1000);
    CHECK_EQ(c->count("frame_ready 2"), size_t(0));
    // A new window is damage: the second frame arrives, with damage.
    size_t damage_before = c->count("damage ");
    auto g = window(host, "green", "FF00FF00", "100x80");
    REQUIRE(g);
    CHECK(c->wait_line("frame_ready 2", 5000));
    CHECK(c->count("damage ") > damage_before);
    g->send("quit\n");
    g->wait_exit(5000);
    c->kill_now();
}

void window_session(Host& host) {
    std::printf("-- ext-image-copy-capture: foreign-toplevel source (host-rendered)\n");
    uint64_t answered = host.captures_answered();
    auto c = Child::spawn({BC_WL_SESSION_CLIENT, "--capture", "toplevel:red", "--frames", "2", "--pixel", "5,5",
                           "--pixel", "290,190"},
                          host.client_env());
    REQUIRE(c);
    CHECK(c->wait_line("session 300 200", 5000));
    REQUIRE(c->wait_line("frame_ready 1", 5000));
    CHECK_EQ(pixel_line(*c, 5, 5), std::optional<uint32_t>(kRed));
    CHECK_EQ(pixel_line(*c, 290, 190), std::optional<uint32_t>(kRed));
    CHECK(host.captures_answered() > answered);
    // The window goes away: the pending frame fails and the session stops.
    auto red = host.window_by_app_id("red");
    host.server().close(red);
    CHECK(c->wait_line("stopped", 5000));
    CHECK_EQ(c->count("frame_ready 2"), size_t(0));
}

void host_copies() {
    std::printf("-- host_capture_copies\n");
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.xwayland = XwaylandMode::Off;
    o.server.host_capture_copies = true;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        CHECK(false);
        return;
    }
    auto w = window(host, "blue", "FF0000FF");
    REQUIRE(w);
    Rect f = frame_of(host, "blue");
    MonitorId out = host.wm_monitors().at(0).id;
    CHECK(host.wait([&] { return host.output_pixel(out, uint32_t(f.x + 10), uint32_t(f.y + 10)) == 0xFF0000FFu; }));
    std::string px = std::to_string(f.x + 10) + "," + std::to_string(f.y + 10);
    auto c = Child::spawn({BC_WL_SESSION_CLIENT, "--capture", "output", "--pixel", px}, host.client_env());
    REQUIRE(c);
    REQUIRE(c->wait_line("frame_ready 1", 5000));
    CHECK_EQ(pixel_line(*c, f.x + 10, f.y + 10), std::optional<uint32_t>(0xFF0000FFu));
    CHECK(host.captures_answered() >= 1);
    c->kill_now();
    host.stop();
}

void server_copies(const std::string& dir) {
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.output_buffers = OutputBufferKind::Shm;
    o.server.xwayland = XwaylandMode::Off;
    o.track_damage = true;  // clients keep committing; only real changes are damage
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        CHECK(false);
        return;
    }
    auto red = window(host, "red", "FFFF0000");
    REQUIRE(red);
    grim(host, dir);
    output_session(host);
    window_session(host);
    host.stop();
}

}  // namespace

int main() {
    std::string dir = private_runtime_dir();
    if (dir.empty()) return 1;
    server_copies(dir);
    host_copies();
    return finish("test_wl_capture");
}
