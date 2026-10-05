// ScreenCaptureKit window capture into Vulkan, pixel-checked: a test
// application's solid-colour window is captured, imported through MoltenVK
// and read back. Without Screen Recording the capture must refuse with a
// reason naming the permission (checked), and the rest is skipped.
#include "brocompositor/mac/capture.h"
#include "harness.h"
#include "vk_mac.h"

#include <cstdlib>
#include <thread>

using namespace bctest;
using namespace brocompositor;

namespace {

bool close_to(uint32_t a, uint32_t b, int tolerance) {
    for (int s = 0; s < 24; s += 8)
        if (std::abs(int((a >> s) & 255) - int((b >> s) & 255)) > tolerance) return false;
    return true;
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
        CHECK(err.find("Screen Recording") != std::string::npos);
        require_screen_recording();  // skips (or fails if the check above failed)
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
    auto image = importer->import_image(*desc, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &err);
    if (!timeline || !image) {
        std::fprintf(stderr, "import: %s\n", err.c_str());
        return 1;
    }
    auto px = vk.read(*importer, *image, timeline->semaphore, frame->wait_value, &err);
    CHECK(px.has_value());
    if (px) {
        // The centre of the content (clear of the title bar, which is at the top).
        uint32_t x = uint32_t(frame->content.width / 2), y = uint32_t(frame->content.height * 2 / 3);
        const uint8_t* p = &(*px)[(size_t(y) * image->extent.width + x) * 4];
        uint32_t got = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        std::printf("   centre pixel %06X (window colour D04020)\n", got);
        CHECK(close_to(got, 0xD04020, 40));  // colour management may shift it slightly
    }
    cap->release(*frame);
    importer->destroy(*image);
    importer->destroy(*timeline);

    // The window goes away: the capture reports closed.
    CHECK(app.ok("close captured"));
    CHECK(eventually([&] { return cap->closed(); }, 5000ms));
    return finish("test_mac_capture");
}
