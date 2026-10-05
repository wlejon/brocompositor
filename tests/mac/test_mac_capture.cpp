// ScreenCaptureKit window capture into Vulkan, pixel-checked: a test
// application's solid-colour window is captured, imported through MoltenVK
// and read back; with Accessibility the window is then parked by the shell
// backend and must keep delivering frames (new content, new colour) while
// parked. Without Screen Recording the capture must refuse with a reason
// naming the permission and the binary to grant it to (checked), and the
// rest is skipped.
#include "brocompositor/mac/capture.h"
#include "harness.h"
#include "vk_mac.h"

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

// Parked by the shell backend, the window keeps being captured: new content
// arrives as new frames.
void capture_while_parked(TestApp& app, uint32_t cgid, mac::WindowCapture& cap,
                          const std::function<std::optional<uint32_t>(const Frame&)>& centre_pixel,
                          uint64_t& last_sequence) {
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
    std::optional<uint32_t> parked_pixel;
    eventually([&] {
        auto f = cap.acquire();
        if (!f) return false;
        if (f->sequence > last_sequence) {
            last_sequence = f->sequence;
            auto px = centre_pixel(*f);
            if (px && close_to(*px, 0x20B040, 40)) parked_pixel = px;
        }
        cap.release(*f);
        return parked_pixel.has_value();
    }, 5000ms);
    CHECK(parked_pixel.has_value());
    if (parked_pixel) std::printf("   parked centre pixel %06X (new colour 20B040)\n", *parked_pixel);
    auto shown = backend->set_visible(added->window.id, true);
    CHECK(shown.wait_for(5s) == std::future_status::ready && shown.get());
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
    auto centre_pixel = [&](const Frame& f) -> std::optional<uint32_t> {
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
        CHECK(close_to(*got, 0xD04020, 40));  // colour management may shift it slightly
    }
    uint64_t last_sequence = frame->sequence;
    cap->release(*frame);

    if (mac_permissions().accessibility) capture_while_parked(app, cgid, *cap, centre_pixel, last_sequence);

    for (auto& [id, img] : images) importer->destroy(img);
    importer->destroy(*timeline);

    // The window goes away: the capture reports closed.
    CHECK(app.ok("close captured"));
    CHECK(eventually([&] { return cap->closed(); }, 5000ms));
    return finish("test_mac_capture");
}
