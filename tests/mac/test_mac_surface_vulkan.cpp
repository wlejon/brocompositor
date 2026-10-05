// The macOS surface contract end to end, without any permission: IOSurfaces
// fed through a SurfaceRing (the path ScreenCaptureKit frames take) are
// imported into Vulkan through MoltenVK (VK_EXT_metal_objects), the
// MTLSharedEvent timeline orders the Metal copy before the Vulkan read, and
// the pixels read back are the pixels submitted. Also: leases are never
// overwritten (frames drop instead), a resize makes a new image set while
// leased old images stay valid, content size is reported, adapters match.
#include "brocompositor/mac/capture.h"
#include "brocompositor/vulkan/importer.h"
#include "check.h"
#include "vk_mac.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#include <cstdio>
#include <cstdlib>
#include <map>

using namespace bctest;
using namespace brocompositor;
using BSize = brocompositor::Size;  // MacTypes.h has a ::Size too

namespace {

struct Surface {
    IOSurfaceRef ref = nullptr;
    ~Surface() {
        if (ref) CFRelease(ref);
    }
};

void put(CFMutableDictionaryRef d, CFStringRef key, int32_t v) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

// A BGRA IOSurface: left half `left`, right half `right` (0xRRGGBB).
IOSurfaceRef make_source(int32_t w, int32_t h, uint32_t left, uint32_t right) {
    CFMutableDictionaryRef props =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    put(props, kIOSurfaceWidth, w);
    put(props, kIOSurfaceHeight, h);
    put(props, kIOSurfaceBytesPerElement, 4);
    put(props, kIOSurfacePixelFormat, int32_t('BGRA'));
    IOSurfaceRef s = IOSurfaceCreate(props);
    CFRelease(props);
    if (!s) return nullptr;
    IOSurfaceLock(s, 0, nullptr);
    auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(s));
    size_t stride = IOSurfaceGetBytesPerRow(s);
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            uint32_t c = x < w / 2 ? left : right;
            uint8_t* p = base + size_t(y) * stride + size_t(x) * 4;
            p[0] = uint8_t(c), p[1] = uint8_t(c >> 8), p[2] = uint8_t(c >> 16), p[3] = 0xFF;
        }
    IOSurfaceUnlock(s, 0, nullptr);
    return s;
}

uint32_t pixel(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t x, uint32_t y) {
    const uint8_t* p = &bgra[(size_t(y) * width + x) * 4];
    return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

struct Host {
    VkMac vk;
    std::unique_ptr<vk::Importer> importer;
    std::optional<vk::ImportedTimeline> timeline;
    std::map<uint64_t, vk::ImportedImage> images;

    ~Host() {
        if (!importer) return;
        for (auto& [id, img] : images) importer->destroy(img);
        if (timeline) importer->destroy(*timeline);
    }

    // Imports (once) and reads back the frame's image.
    std::optional<std::vector<uint8_t>> read(SurfaceSource& src, const Frame& f, uint32_t* width) {
        std::string why;
        if (!timeline) {
            SharedTimeline t = src.timeline();
            CHECK(importer->supports(t));
            timeline = importer->import_timeline(t, &why);
            if (!timeline) {
                std::fprintf(stderr, "import_timeline: %s\n", why.c_str());
                return std::nullopt;
            }
        }
        auto it = images.find(f.image_id);
        if (it == images.end()) {
            auto desc = src.image(f.image_id);
            if (!desc) return std::nullopt;
            CHECK(importer->supports(*desc, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
            auto img = importer->import_image(*desc, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &why);
            if (!img) {
                std::fprintf(stderr, "import_image: %s\n", why.c_str());
                return std::nullopt;
            }
            CHECK(img->external_queue_family == VK_QUEUE_FAMILY_IGNORED);
            it = images.emplace(f.image_id, *img).first;
        }
        *width = it->second.extent.width;
        auto out = vk.read(*importer, it->second, timeline->semaphore, f.wait_value, &why);
        if (!out) std::fprintf(stderr, "read: %s\n", why.c_str());
        return out;
    }
};

}  // namespace

int main() {
    std::string err;
    auto device = mac::MetalDevice::create({}, &err);
    if (!device) {
        std::printf("SKIP: %s\n", err.c_str());
        return 77;
    }
    Host host;
    AdapterId want = device->adapter();
    if (!host.vk.init(&want, &err)) {
        std::printf("SKIP: %s\n", err.c_str());
        return 77;
    }
    std::printf("-- adapter %llx\n", (unsigned long long)want.metal_registry_id);
    CHECK(want.metal_registry_id != 0);
    CHECK(host.vk.adapter() == want);  // MoltenVK's deviceLUID decodes to the Metal registryID
    host.importer = vk::Importer::create(host.vk.context(), &err);
    if (!host.importer) {
        std::fprintf(stderr, "Importer::create: %s\n", err.c_str());
        return 1;
    }
    auto ring = mac::SurfaceRing::create(device, {}, &err);
    if (!ring) {
        std::fprintf(stderr, "SurfaceRing::create: %s\n", err.c_str());
        return 1;
    }
    CHECK(!ring->acquire());
    CHECK(ring->timeline().type == SyncHandleType::MetalSharedEvent);

    std::printf("-- frames through Vulkan\n");
    Surface a{make_source(256, 128, 0xC03020, 0x2040D0)};
    Surface b{make_source(256, 128, 0x20B040, 0xE0E0E0)};
    if (!a.ref || !b.ref) return 1;
    CHECK(ring->submit(a.ref, BSize{256, 128}, 1));
    auto f1 = ring->acquire();
    if (!f1) return finish("test_mac_surface_vulkan");
    auto img1 = ring->image(f1->image_id);
    CHECK(img1 && img1->type == ImageHandleType::IOSurface && img1->width == 256 && img1->height == 128);
    CHECK(img1 && img1->iosurface_id != 0 && img1->adapter == want);
    CHECK(f1->content == (BSize{256, 128}));
    uint32_t w = 0;
    auto px = host.read(*ring, *f1, &w);
    CHECK(px.has_value());
    if (px) {
        CHECK_EQ(pixel(*px, w, 10, 10), 0xC03020u);
        CHECK_EQ(pixel(*px, w, 200, 100), 0x2040D0u);
    }
    ring->release(*f1);

    CHECK(ring->submit(b.ref, BSize{256, 128}, 2));
    auto f2 = ring->acquire();
    if (!f2) return finish("test_mac_surface_vulkan");
    CHECK(f2->sequence > f1->sequence);
    CHECK(f2->wait_value > f1->wait_value);
    CHECK(f2->image_id != f1->image_id);  // never the slot the latest frame is in
    px = host.read(*ring, *f2, &w);
    if (px) {
        CHECK_EQ(pixel(*px, w, 10, 10), 0x20B040u);
        CHECK_EQ(pixel(*px, w, 200, 100), 0xE0E0E0u);
    }

    std::printf("-- leases are never overwritten\n");
    // f2 leased (and latest). Fill the other slots, leasing each.
    CHECK(ring->submit(a.ref, BSize{256, 128}, 3));
    auto f3 = ring->acquire();
    CHECK(ring->submit(a.ref, BSize{256, 128}, 4));
    auto f4 = ring->acquire();
    if (!f3 || !f4) return finish("test_mac_surface_vulkan") | 1;
    uint64_t dropped = ring->frames_dropped();
    CHECK(!ring->submit(b.ref, BSize{256, 128}, 5));  // every slot leased or latest
    CHECK_EQ(ring->frames_dropped(), dropped + 1);
    px = host.read(*ring, *f2, &w);  // the leased frame still holds its pixels
    if (px) CHECK_EQ(pixel(*px, w, 10, 10), 0x20B040u);
    ring->release(*f3);
    CHECK(ring->submit(b.ref, BSize{256, 128}, 6));

    std::printf("-- resize\n");
    uint64_t gen = ring->images_generation();
    uint64_t old_id = f2->image_id;
    Surface small{make_source(128, 64, 0x102030, 0x405060)};
    if (!small.ref) return 1;
    CHECK(ring->submit(small.ref, BSize{100, 50}, 7));
    CHECK(ring->images_generation() > gen);
    for (const auto& i : ring->images()) CHECK(i.width == 128 && i.height == 64);
    CHECK(ring->image(old_id).has_value());  // leased: kept until released
    auto f5 = ring->acquire();
    if (!f5) return finish("test_mac_surface_vulkan") | 1;
    CHECK(f5->content == (BSize{100, 50}));
    px = host.read(*ring, *f5, &w);
    if (px) {
        CHECK_EQ(w, 128u);
        CHECK_EQ(pixel(*px, w, 10, 10), 0x102030u);
        CHECK_EQ(pixel(*px, w, 90, 40), 0x405060u);
    }
    ring->release(*f2);
    ring->release(*f4);
    CHECK(!ring->image(old_id).has_value());
    ring->release(*f5);
    ring->close();
    CHECK(ring->closed());
    CHECK(!ring->submit(small.ref, BSize{128, 64}, 8));
    return finish("test_mac_surface_vulkan");
}
