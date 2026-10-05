// DRM/KMS + session + libinput: the server role on a real KMS device.
// Opt-in, because it takes DRM master: set BROCOMPOSITOR_DRM_DEVICE to a
// card node that drives no display anyone is using -- vkms is the intended
// target (`sudo modprobe vkms`; the node is the card whose driver is vkms) --
// and make a libseat backend available (seatd with the user in its group,
// or logind on an active seat). Without it the test skips (exit 77).
//
// Checks: the connector becomes a monitor with its modes, the host's
// composite is page-flipped at the vblank pace (OutputPresented with the
// kernel's timestamps), a client maps and is drawn into the scanout image,
// mode changes reallocate the image set, and disable/enable round-trips.
#include "linux/wl_harness.h"
#include "printers.h"

#include <cstdlib>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

size_t presented_on(Host& host, MonitorId m) {
    size_t n = 0;
    for (auto& p : host.server_events_of<OutputPresented>())
        if (p.output == m && p.presented) ++n;
    return n;
}

void run(Host& host) {
    REQUIRE(host.wait([&] { return !host.wm_monitors().empty(); }, 10000));
    MonitorId m = host.wm_monitors()[0].id;
    std::optional<OutputInfo> info;
    for (auto& o : host.server().outputs())
        if (o.id == m) info = o;
    REQUIRE(info);
    std::printf("output %s (%s): %dx%d@%d mHz, %zu modes\n", info->name.c_str(), info->description.c_str(),
                info->pixel_size.width, info->pixel_size.height, info->refresh_mhz, info->modes.size());
    CHECK(!info->modes.empty());
    CHECK(info->refresh_mhz > 0);
    auto imgs = host.server().output_images(m);
    REQUIRE(!imgs.empty());
    std::printf("scanout images: %zu x %s modifier %llx\n", imgs.size(),
                imgs[0].type == ImageHandleType::DmaBuf ? "dmabuf" : "shm",
                static_cast<unsigned long long>(imgs[0].drm_modifier));

    // Page flips at the vblank pace with kernel timestamps.
    CHECK(host.wait([&] { return presented_on(host, m) >= 30; }, 5000));
    auto pres = host.server_events_of<OutputPresented>();
    int64_t last = 0;
    bool monotonic = true;
    uint32_t refresh = 0;
    for (auto& p : pres) {
        if (p.output != m || !p.presented) continue;
        if (p.timestamp_ns < last) monotonic = false;
        last = p.timestamp_ns;
        refresh = p.refresh_ns;
    }
    CHECK(monotonic);
    CHECK(refresh > 10000000 && refresh < 40000000);
    CHECK(host.server_events_of<OutputPresentFailed>().empty());

    // A client, drawn into scanout.
    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "kms", "--size", "160x120", "--color", "FF4080C0", "--animate"},
                          host.client_env());
    REQUIRE(c);
    REQUIRE(host.wait([&] { return host.window_by_app_id("kms") != kNoWindow; }));
    WindowId w = host.window_by_app_id("kms");
    Rect f = host.server().query(w)->frame;
    CHECK(host.wait([&] {
        return host.output_pixel(m, uint32_t(f.x + 80), uint32_t(f.y + 60)) == 0xFF4080C0u;
    }));
    CHECK(c->wait_count("frame ", 30, 5000));
    {
        auto px = host.output_pixel(m, uint32_t(f.x + 80), uint32_t(f.y + 60));
        std::printf("client frames %zu, output pixel %s, drawn %zu, presents %llu\n", c->count("frame "),
                    px ? std::to_string(*px).c_str() : "unmappable", host.last_drawn(m).size(),
                    static_cast<unsigned long long>(host.presents(m)));
    }

    // Mode change: pick a different listed mode.
    std::optional<OutputMode> other;
    for (auto& md : info->modes)
        if (md.size != info->pixel_size && md.size.width >= 640 && md.size.width <= 1280) other = md;
    if (other) {
        OutputConfig oc;
        oc.mode = other->size;
        oc.refresh_mhz = other->refresh_mhz;
        CHECK(host.server().configure_output(m, oc));
        CHECK(host.wait([&] {
            for (auto& o : host.server().outputs())
                if (o.id == m) return o.pixel_size == other->size;
            return false;
        }));
        for (auto& im : host.server().output_images(m))
            CHECK_EQ((Size{int32_t(im.width), int32_t(im.height)}), other->size);
        size_t before = presented_on(host, m);
        CHECK(host.wait([&] { return presented_on(host, m) >= before + 10; }));
    }

    // Disable / enable.
    OutputConfig off;
    off.enabled = false;
    CHECK(host.server().configure_output(m, off));
    CHECK(host.wait([&] { return host.wm_monitors().empty(); }));
    OutputConfig on;
    on.enabled = true;
    CHECK(host.server().configure_output(m, on));
    CHECK(host.wait([&] { return host.wm_monitors().size() == 1; }));
    size_t before = presented_on(host, m);
    CHECK(host.wait([&] { return presented_on(host, m) >= before + 10; }));
    c->kill_now();
}

}  // namespace

int main() {
    const char* dev = std::getenv("BROCOMPOSITOR_DRM_DEVICE");
    if (!dev || !*dev) {
        std::printf("SKIP: set BROCOMPOSITOR_DRM_DEVICE to an unused KMS card (vkms)\n");
        return 77;
    }
    if (private_runtime_dir().empty()) return 1;
    setenv("WLR_DRM_DEVICES", dev, 1);
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Drm;
    std::string err;
    if (!host.start(o, &err)) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    run(host);
    host.stop();
    return finish("test_wl_drm");
}
