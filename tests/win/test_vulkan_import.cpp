// End-to-end proof of the surface contract: a foreign process's window is
// captured with Windows.Graphics.Capture, its shared D3D11 textures and
// shared fence are imported into a Vulkan device (zero-copy), Vulkan waits on
// the fence as a timeline semaphore, copies the image and the pixels match
// what the window painted. Then the window changes colour and is resized, and
// the host re-imports the new generation.
#include "brocompositor/vulkan/importer.h"
#include "brocompositor/win/capture.h"

#include "check.h"
#include "printers.h"
#include "win/harness.h"
#include "win/vk_context.h"

#include <cstdio>
#include <map>

using namespace brocompositor;
using namespace bctest;

namespace {

uint32_t rgb_at(const std::vector<uint8_t>& px, uint32_t width, uint32_t x, uint32_t y) {
    const uint8_t* p = px.data() + (size_t(y) * width + x) * 4;  // BGRA
    return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

struct Host {
    VkContext& vk;
    vk::Importer& importer;
    win::WindowCapture& cap;
    std::map<uint64_t, vk::ImportedImage> images;  // import cache keyed by SharedImage::id
    std::optional<vk::ImportedTimeline> timeline;

    ~Host() {
        for (auto& [id, img] : images) importer.destroy(img);
        if (timeline) importer.destroy(*timeline);
    }

    // One host frame: lease the newest frame, import lazily, read it.
    std::optional<std::pair<Frame, uint32_t>> sample(uint64_t after) {
        auto f = cap.acquire();
        if (!f) return std::nullopt;
        std::optional<std::pair<Frame, uint32_t>> out;
        if (f->sequence > after) {
            std::string err;
            if (!timeline) timeline = importer.import_timeline(cap.timeline(), &err);
            auto it = images.find(f->image_id);
            if (it == images.end()) {
                auto desc = cap.image(f->image_id);
                auto img = desc ? importer.import_image(*desc, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                                   VK_IMAGE_USAGE_SAMPLED_BIT, &err)
                                : std::nullopt;
                if (img) it = images.emplace(f->image_id, *img).first;
            }
            if (timeline && it != images.end()) {
                auto px = vk.read_back(importer, it->second, *timeline, f->wait_value, &err);
                if (!px.empty()) {
                    uint32_t w = it->second.extent.width;
                    out = std::pair{*f, rgb_at(px, w, uint32_t(f->content.width) / 2,
                                               uint32_t(f->content.height) / 2)};
                }
            }
            if (!err.empty()) std::fprintf(stderr, "  import/read: %s\n", err.c_str());
        }
        cap.release(*f);  // read_back waited for the GPU work
        return out;
    }

    // Drops imports that left the current image set (after a resize).
    void evict_retired() {
        auto current = cap.images();
        for (auto it = images.begin(); it != images.end();) {
            bool live = false;
            for (auto& i : current) live |= i.id == it->first;
            if (live) {
                ++it;
            } else {
                importer.destroy(it->second);
                it = images.erase(it);
            }
        }
    }

    std::optional<Frame> wait_color(uint64_t after, uint32_t rgb, uint32_t* seen) {
        std::optional<Frame> hit;
        wait_until([&] {
            auto s = sample(after);
            if (s) *seen = s->second;
            if (s && s->second == rgb) hit = s->first;
            return hit.has_value();
        }, 5000ms);
        return hit;
    }
};

int run() {
    VkContext vkc;
    std::string why;
    if (!vkc.init(&why)) {
        std::printf("[test_vulkan_import] SKIPPED: %s\n", why.c_str());
        return 77;
    }
    std::printf("  Vulkan device: %s\n", vkc.device_name().c_str());
    std::string err;
    auto importer = vk::Importer::create(vkc.device_context(), &err);
    if (!importer) std::fprintf(stderr, "Importer: %s\n", err.c_str());
    CHECK(importer != nullptr);
    if (!importer) return 1;

    TestApp app;
    if (!app.start()) return 1;
    Rect wa = primary_work_area();
    HWND h = app.create("vk", Rect{wa.x + 150, wa.y + 120, 720, 520}, "e01070");
    if (!h) return 1;

    // The D3D11 producer is created on the Vulkan device's adapter.
    win::CaptureDeviceConfig dcfg;
    dcfg.adapter = vkc.adapter();
    auto device = win::CaptureDevice::create(dcfg, &err);
    if (!device) std::fprintf(stderr, "CaptureDevice: %s\n", err.c_str());
    CHECK(device != nullptr);
    if (!device) return 1;
    CHECK(device->adapter() == vkc.adapter());
    auto cap = win::WindowCapture::start(device, uint64_t(reinterpret_cast<uintptr_t>(h)), {}, &err);
    if (!cap) std::fprintf(stderr, "WindowCapture: %s\n", err.c_str());
    CHECK(cap != nullptr);
    if (!cap) return 1;

    CHECK(importer->supports(cap->timeline()));
    for (auto& img : cap->images()) CHECK(importer->supports(img, VK_IMAGE_USAGE_TRANSFER_SRC_BIT));

    Host host{vkc, *importer, *cap, {}, std::nullopt};
    uint32_t seen = 0;
    auto f1 = host.wait_color(0, 0xe01070, &seen);
    if (!f1) std::fprintf(stderr, "  last colour seen %06x\n", seen);
    CHECK(f1.has_value());

    CHECK(app.ok("color vk 10c040"));
    auto f2 = host.wait_color(f1 ? f1->sequence : 0, 0x10c040, &seen);
    CHECK(f2.has_value());
    // Images are imported once and reused across frames.
    CHECK(host.images.size() <= 3);

    uint64_t gen = cap->images_generation();
    CHECK(app.ok("move vk " + std::to_string(wa.x + 150) + " " + std::to_string(wa.y + 120) + " 1000 700"));
    CHECK(app.ok("color vk 2050ff"));
    auto f3 = host.wait_color(f2 ? f2->sequence : 0, 0x2050ff, &seen);
    CHECK(f3.has_value());
    CHECK(cap->images_generation() > gen);
    host.evict_retired();
    if (f3) {
        auto it = host.images.find(f3->image_id);
        CHECK(it != host.images.end() && it->second.extent.width > 720);
    }
    std::printf("  frames: %llu, imports live: %zu, dropped: %llu\n",
                f3 ? (unsigned long long)f3->sequence : 0ull, host.images.size(),
                (unsigned long long)cap->frames_dropped());
    return 0;
}

}  // namespace

int main() {
    init_windows_test();
    int rc = run();
    if (rc == 77) return 77;
    return finish("test_vulkan_import");
}
