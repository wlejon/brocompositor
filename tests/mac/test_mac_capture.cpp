// ScreenCaptureKit window capture into Vulkan, pixel-checked: a test
// application's solid-colour window is captured, imported through MoltenVK
// and read back (sRGB output: the window's colour comes back exactly, not
// shifted into the display's P3). Then, through the same capture:
//   * parked by the shell backend (Accessibility; 1 x 91 pt left on screen)
//     the window keeps delivering whole frames with new content;
//   * minimized, ScreenCaptureKit suspends the stream and the capture stays
//     open; deminiaturized, new content arrives again;
//   * closed, the capture reports closed (Accessibility: the window server
//     keeps a closed window, and ScreenCaptureKit treats it like an ordered
//     out one; without the permission it honestly cannot tell);
//   * a second capture reports closed when the application exits.
// Without Screen Recording the capture must refuse with a reason naming the
// permission and the binary to grant it to (checked), and the rest is
// skipped.
#include "brocompositor/mac/capture.h"
#include "harness.h"
#include "vk_mac.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <thread>

using namespace bctest;
using namespace brocompositor;

namespace {

bool close_to(uint32_t a, uint32_t b, int tolerance) {
    for (int s = 0; s < 24; s += 8)
        if (std::abs(int((a >> s) & 255) - int((b >> s) & 255)) > tolerance) return false;
    return true;
}

// The stream is sRGB: allow only rounding.
constexpr int kTolerance = 6;

using CentrePixel = std::function<std::optional<uint32_t>(const Frame&)>;

struct Seen {
    uint32_t pixel = 0;
    Size content;
};

// Waits for a frame newer than `last_sequence` whose centre is `colour`.
std::optional<Seen> wait_colour(mac::WindowCapture& cap, const CentrePixel& centre_pixel, uint64_t& last_sequence,
                                uint32_t colour) {
    std::optional<Seen> seen;
    eventually([&] {
        auto f = cap.acquire();
        if (!f) return false;
        if (f->sequence > last_sequence) {
            last_sequence = f->sequence;
            auto px = centre_pixel(*f);
            if (px && close_to(*px, colour, kTolerance)) seen = Seen{*px, f->content};
        }
        cap.release(*f);
        return seen.has_value();
    }, 5000ms);
    return seen;
}

// closed() stays false for `period`.
bool stays_open(mac::WindowCapture& cap, std::chrono::milliseconds period) {
    auto end = std::chrono::steady_clock::now() + period;
    while (std::chrono::steady_clock::now() < end) {
        if (cap.closed()) return false;
        std::this_thread::sleep_for(50ms);
    }
    return true;
}

// Parked by the shell backend, the window keeps being captured whole: new
// content arrives as new frames of the full window size.
void capture_while_parked(TestApp& app, uint32_t cgid, mac::WindowCapture& cap, const CentrePixel& centre_pixel,
                          uint64_t& last_sequence, Size content) {
    std::printf("-- capture while parked\n");
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    auto added = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == cgid; });
    REQUIRE(added);
    auto hidden = backend->set_visible(added->window.id, false);
    REQUIRE(hidden.wait_for(5s) == std::future_status::ready);
    CHECK(hidden.get());
    REQUIRE(app.ok("color captured 20B040"));
    auto parked = wait_colour(cap, centre_pixel, last_sequence, 0x20B040);
    CHECK(parked.has_value());
    if (parked) {
        std::printf("   parked centre pixel %06X (new colour 20B040), content %dx%d\n", parked->pixel,
                    parked->content.width, parked->content.height);
        // The whole window (320 x 200 pt), not the sliver left on screen.
        CHECK(std::abs(parked->content.width - content.width) <= 2);
        CHECK(std::abs(parked->content.height - content.height) <= 2);
    }
    CHECK(!cap.closed());
    auto shown = backend->set_visible(added->window.id, true);
    CHECK(shown.wait_for(5s) == std::future_status::ready && shown.get());
}

// Minimized: the stream is suspended, the capture is not closed, and frames
// resume after deminiaturizing.
void capture_across_minimize(TestApp& app, mac::WindowCapture& cap, const CentrePixel& centre_pixel,
                             uint64_t& last_sequence) {
    std::printf("-- minimize / deminiaturize\n");
    REQUIRE(app.ok("minimize captured"));
    CHECK(stays_open(cap, 1500ms));
    REQUIRE(app.ok("unminimize captured"));
    REQUIRE(app.ok("color captured 2040B0"));
    auto back = wait_colour(cap, centre_pixel, last_sequence, 0x2040B0);
    CHECK(back.has_value());
    if (back) std::printf("   after deminiaturize centre pixel %06X (new colour 2040B0)\n", back->pixel);
    CHECK(!cap.closed());
}

}  // namespace

int main() {
    mac_permissions();
    require_display();
    std::string err;
    auto device = mac::MetalDevice::create({}, &err);
    if (!device) skip("no Metal device: " + err);
    TestApp app;
    if (!app.start()) return 1;
    uint32_t cgid = app.create("captured", Rect{220, 220, 320, 200}, "D04020");
    if (!cgid) return 1;

    if (!mac_permissions().screen_recording) {
        std::printf("-- refusal without Screen Recording\n");
        CHECK(!mac::capture_supported());
        auto none = mac::WindowCapture::start(device, cgid, {}, &err);
        CHECK(!none);
        std::printf("   %s\n", err.c_str());
        CHECK(err.find("Screen Recording") != std::string::npos);
        CHECK(err.find(mac_permissions().responsible_path) != std::string::npos);
        require_screen_recording();  // skips (or fails if the checks above failed)
    }
    require_unlocked();

    std::printf("-- capture\n");
    CHECK(mac::capture_supported());
    auto cap = mac::WindowCapture::start(device, cgid, {}, &err);
    if (!cap) {
        std::fprintf(stderr, "WindowCapture::start: %s\n", err.c_str());
        return 1;
    }
    std::optional<Frame> frame;
    eventually([&] { return (frame = cap->acquire()).has_value(); }, 5000ms);
    if (!frame) {
        CHECK(!"no frame within 5 s");
        return finish("test_mac_capture");
    }
    auto desc = cap->image(frame->image_id);
    if (!desc) return 1;
    CHECK(desc->type == ImageHandleType::IOSurface);
    std::printf("   frame %ux%u content %dx%d\n", desc->width, desc->height, frame->content.width,
                frame->content.height);
    CHECK(desc->width >= 320 && desc->height >= 200);  // pixels: points x scale

    VkMac vk;
    AdapterId want = device->adapter();
    if (!vk.init(&want, &err)) skip(err);
    auto importer = vk::Importer::create(vk.context(), &err);
    if (!importer) return 1;
    auto timeline = importer->import_timeline(cap->timeline(), &err);
    if (!timeline) {
        std::fprintf(stderr, "import timeline: %s\n", err.c_str());
        return 1;
    }
    std::map<uint64_t, vk::ImportedImage> images;  // image id -> import (ring slots, imported once)
    // The centre of the frame's content (clear of the title bar at the top).
    CentrePixel centre_pixel = [&](const Frame& f) -> std::optional<uint32_t> {
        auto it = images.find(f.image_id);
        if (it == images.end()) {
            auto d = cap->image(f.image_id);
            if (!d) return std::nullopt;
            auto img = importer->import_image(*d, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &err);
            if (!img) return std::nullopt;
            it = images.emplace(f.image_id, *img).first;
        }
        auto px = vk.read(*importer, it->second, timeline->semaphore, f.wait_value, &err);
        if (!px) return std::nullopt;
        uint32_t x = uint32_t(f.content.width / 2), y = uint32_t(f.content.height * 2 / 3);
        const uint8_t* p = &(*px)[(size_t(y) * it->second.extent.width + x) * 4];
        return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    };
    auto got = centre_pixel(*frame);
    CHECK(got.has_value());
    if (got) {
        std::printf("   centre pixel %06X (window colour D04020)\n", *got);
        CHECK(close_to(*got, 0xD04020, kTolerance));
    }
    uint64_t last_sequence = frame->sequence;
    cap->release(*frame);
    // The window in pixels. (Not the first frame's size: that one can be
    // from the window's opening animation, a few percent smaller.)
    double scale = cg_display_dpi(cg_main_display()) / 96.0;
    Size content{int32_t(std::lround(320 * scale)), int32_t(std::lround(200 * scale))};

    if (mac_permissions().accessibility)
        capture_while_parked(app, cgid, *cap, centre_pixel, last_sequence, content);
    capture_across_minimize(app, *cap, centre_pixel, last_sequence);

    for (auto& [id, img] : images) importer->destroy(img);
    importer->destroy(*timeline);

    // The window is closed. The window server keeps it (off screen) for as
    // long as its process lives and the stream only suspends, as for an
    // ordered-out window; Accessibility tells closed from minimized.
    std::printf("-- close\n");
    CHECK(app.ok("close captured"));
    if (mac_permissions().accessibility) {
        CHECK(eventually([&] { return cap->closed(); }, 5000ms));
    } else {
        std::printf("   no Accessibility: a closed window is indistinguishable from an ordered-out one\n");
        CHECK(stays_open(*cap, 1000ms));
    }

    // The application exits: its windows leave the window server.
    std::printf("-- application exit\n");
    uint32_t second = app.create("second", Rect{260, 260, 240, 160}, "D04020");
    CHECK(second);
    auto cap2 = second ? mac::WindowCapture::start(device, second, {}, &err) : nullptr;
    if (!cap2) {
        std::fprintf(stderr, "WindowCapture::start: %s\n", err.c_str());
        CHECK(!"second capture");
        return finish("test_mac_capture");
    }
    CHECK(eventually([&] {
        auto f = cap2->acquire();
        if (f) cap2->release(*f);
        return f.has_value();
    }, 5000ms));
    CHECK(!cap2->closed());
    app.kill_now();
    CHECK(eventually([&] { return cap2->closed(); }, 5000ms));
    return finish("test_mac_capture");
}
