// dmabuf <-> Vulkan through the server role, on every Vulkan device that has
// the importer's extensions (the GPU, and lavapipe as the no-GPU fallback):
//   * the host advertises Importer::dmabuf_formats() to the server, so a
//     client's linux-dmabuf allocation (GBM on the GPU; udmabuf LINEAR for a
//     CPU device) is importable;
//   * the client's buffer arrives as a DmaBuf SharedImage with its modifier,
//     imports zero-copy, and reads back the client's colour after waiting the
//     frame's sync_file; a recolour arrives as a new frame;
//   * (GPU on the server's render node) the host renders into the server's
//     dmabuf output images with Vulkan and presents them with a render-done
//     sync_file; every present completes and the image holds the colour.
#include "linux/vk_host.h"
#include "linux/wl_harness.h"
#include "printers.h"

#include <drm_fourcc.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <set>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace bctest;

namespace {

constexpr VkImageUsageFlags kClientUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageUsageFlags kOutputUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

// Reads the client's newest frame through Vulkan; returns the BGRA texel at (x, y).
std::optional<uint32_t> client_texel(VkHost& vkh, vk::Importer& imp, ClientSurface& src, uint64_t min_sequence,
                                     uint32_t x, uint32_t y, SharedImage* seen) {
    auto frame = src.acquire();
    if (!frame || frame->sequence < min_sequence) {
        if (frame) src.release(*frame);
        return std::nullopt;
    }
    std::optional<uint32_t> texel;
    auto img = src.image(frame->image_id);
    std::string why;
    if (img && imp.supports(*img, kClientUsage)) {
        if (seen) *seen = *img;
        if (auto in = imp.import_image(*img, kClientUsage, &why)) {
            auto px = vkh.read_back(imp, *in, frame->sync_fd, &why);
            if (!px.empty()) {
                size_t o = (size_t(y) * in->extent.width + x) * 4;
                texel = uint32_t(px[o]) | uint32_t(px[o + 1]) << 8 | uint32_t(px[o + 2]) << 16 |
                        uint32_t(px[o + 3]) << 24;  // little-endian BGRA = 0xAARRGGBB
            }
            imp.destroy(*in);
        }
    }
    if (!why.empty()) std::fprintf(stderr, "   import/read: %s\n", why.c_str());
    src.release(*frame);
    return texel;
}

void run_device(VkHost& vkh, size_t index) {
    std::string why;
    REQUIRE(vkh.open(index, &why));
    const VkHostDevice& dev = vkh.device();
    auto imp = vk::Importer::create(vkh.device_context(), &why);
    REQUIRE(imp);
    auto formats = imp->dmabuf_formats(kClientUsage);
    std::vector<DmabufFormat> advertised;
    bool argb = false;
    for (auto& f : formats) {
        advertised.push_back(DmabufFormat{f.fourcc, f.modifiers});
        if (f.fourcc == DRM_FORMAT_ARGB8888) argb = true;
    }
    std::printf("-- %s (%s): %zu importable formats\n", dev.name.c_str(), dev.cpu ? "cpu" : "gpu", formats.size());
    CHECK(argb);

    // The host renders into the output images only when they live on its GPU.
    Host host;
    HostOptions o;
    o.server.backend = BackendKind::Headless;
    o.server.initial_output_size = Size{320, 240};
    o.server.dmabuf_formats = advertised;
    o.composite = false;
    std::mutex vk_mutex;
    std::map<uint64_t, vk::ImportedImage> out_imports;
    std::atomic<int> rendered{0}, render_failed{0};
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    {
        std::string probe_err;
        auto probe = ServerBackend::create(o.server, &probe_err);
        bool same_gpu = probe && dev.adapter && probe->adapter().drm_render_node == dev.adapter->drm_render_node;
        probe.reset();
        o.server.output_buffers = same_gpu ? OutputBufferKind::DmaBuf : OutputBufferKind::Shm;
        if (same_gpu) {
            o.render_hook = [&](MonitorId, const SharedImage& img, PresentRequest& req) {
                std::lock_guard<std::mutex> lock(vk_mutex);
                std::string err;
                auto it = out_imports.find(img.id);
                if (it == out_imports.end()) {
                    auto in = imp->import_image(img, kOutputUsage, &err);
                    if (!in) {
                        ++render_failed;
                        std::fprintf(stderr, "   output import: %s\n", err.c_str());
                        return;
                    }
                    it = out_imports.emplace(img.id, *in).first;
                }
                auto fd = vkh.clear(*imp, it->second, clear, &err);
                if (!fd) {
                    ++render_failed;
                    std::fprintf(stderr, "   output clear: %s\n", err.c_str());
                    return;
                }
                req.render_done = *fd;
                ++rendered;
            };
        }
    }
    REQUIRE(host.start(o, &why));
    REQUIRE(host.wait([&] { return host.wm_monitors().size() == 1; }));
    MonitorId mon = host.wm_monitors()[0].id;

    // Client buffer -> Vulkan.
    auto c = Child::spawn({BC_WL_CLIENT, "--app-id", "vk", "--size", "64x48", "--color", "FF3366CC",
                           dev.cpu ? "--udmabuf" : "--dmabuf"},
                          host.client_env());
    REQUIRE(c);
    std::string mod_line;
    bool allocated = c->wait_line("dmabuf ", 5000, &mod_line);
    if (!allocated) std::fprintf(stderr, "client:\n%s\n", c->output().c_str());
    REQUIRE(allocated);
    REQUIRE(c->wait_line("ready", 5000));
    REQUIRE(host.wait([&] { return host.window_by_app_id("vk") != kNoWindow; }));
    SurfaceId root = SurfaceId(host.server().query(host.window_by_app_id("vk"))->native);
    auto src = host.server().surface(root);
    REQUIRE(src);
    SharedImage seen;
    std::optional<uint32_t> texel;
    CHECK(host.wait([&] {
        std::lock_guard<std::mutex> lock(vk_mutex);
        return (texel = client_texel(vkh, *imp, *src, 0, 10, 10, &seen)).has_value();
    }));
    std::printf("   client %s, modifier %s, %zu plane(s): texel %s\n", mod_line.c_str(), hex(seen.drm_modifier).c_str(),
                seen.planes.size(), texel ? hex(*texel).c_str() : "none");
    CHECK(seen.type == ImageHandleType::DmaBuf);
    CHECK_EQ(seen.drm_format, uint32_t(DRM_FORMAT_ARGB8888));
    CHECK_EQ("dmabuf " + hex(seen.drm_modifier), mod_line);
    CHECK_EQ(texel.value_or(0), 0xFF3366CCu);

    // A recolour arrives as a newer frame.
    uint64_t seq = 0;
    if (auto f = src->acquire()) {
        seq = f->sequence;
        src->release(*f);
    }
    c->send("color FF00FF00\n");
    CHECK(host.wait([&] {
        std::lock_guard<std::mutex> lock(vk_mutex);
        texel = client_texel(vkh, *imp, *src, seq + 1, 32, 24, nullptr);
        return texel == 0xFF00FF00u;
    }));

    // A GPU-rendered client (EGL into GBM bos with the driver's tiled
    // modifiers): weston-simple-dmabuf-egl imports and reads back content.
    if (!dev.cpu && !which("weston-simple-dmabuf-egl").empty()) {
        auto egl = Child::spawn({"weston-simple-dmabuf-egl"}, host.client_env(), true);
        REQUIRE(egl);
        WindowId ew = kNoWindow;
        bool mapped = host.wait(
            [&] {
                for (WindowId id : host.server().windows())
                    if (auto s = host.server().query(id); s && s->title.find("dmabuf") != std::string::npos) ew = id;
                return ew != kNoWindow;
            },
            10000);
        if (!mapped) {
            std::printf("   SKIP weston-simple-dmabuf-egl: no window\n%s\n", egl->output().c_str());
        } else {
            auto esrc = host.server().surface(SurfaceId(host.server().query(ew)->native));
            REQUIRE(esrc);
            std::set<uint32_t> distinct;
            SharedImage eimg;
            CHECK(host.wait(
                [&] {
                    std::lock_guard<std::mutex> lock(vk_mutex);
                    auto f = esrc->acquire();
                    if (!f) return false;
                    std::string err;
                    auto img = esrc->image(f->image_id);
                    if (img && imp->supports(*img, kClientUsage)) {
                        eimg = *img;
                        if (auto in = imp->import_image(*img, kClientUsage, &err)) {
                            auto px = vkh.read_back(*imp, *in, f->sync_fd, &err);
                            for (size_t i = 0; i + 3 < px.size(); i += 4 * 97)
                                distinct.insert(uint32_t(px[i]) | uint32_t(px[i + 1]) << 8 |
                                                uint32_t(px[i + 2]) << 16);
                            imp->destroy(*in);
                        }
                    }
                    esrc->release(*f);
                    return distinct.size() >= 2;
                },
                10000));
            std::printf("   weston-simple-dmabuf-egl: %ux%u fourcc %s modifier %s, %zu distinct sampled colours\n",
                        eimg.width, eimg.height, hex(eimg.drm_format).c_str(), hex(eimg.drm_modifier).c_str(),
                        distinct.size());
            CHECK(eimg.type == ImageHandleType::DmaBuf);
        }
        egl->kill_now();
    }

    // Vulkan -> output image -> present (render-done sync_file).
    if (o.server.output_buffers == OutputBufferKind::DmaBuf) {
        CHECK(host.wait([&] { return rendered >= 10; }));
        CHECK_EQ(render_failed.load(), 0);
        CHECK(host.server_events_of<OutputPresentFailed>().empty());
        size_t presented = 0;
        for (auto& p : host.server_events_of<OutputPresented>())
            if (p.output == mon && p.presented) ++presented;
        CHECK(presented >= 5);
        auto imgs = host.server().output_images(mon);
        REQUIRE(!imgs.empty());
        std::printf("   output images: %s modifier %s, %d presents rendered by Vulkan\n",
                    imgs[0].type == ImageHandleType::DmaBuf ? "dmabuf" : "other", hex(imgs[0].drm_modifier).c_str(),
                    rendered.load());
        CHECK(imgs[0].type == ImageHandleType::DmaBuf);
        host.set_presenting(false);
        std::lock_guard<std::mutex> lock(vk_mutex);
        auto it = out_imports.find(imgs[0].id);
        CHECK(it != out_imports.end());
        if (it != out_imports.end()) {
            auto px = vkh.read_back(*imp, it->second, NativeHandle{~uint64_t(0)}, &why);
            REQUIRE(!px.empty());
            // XRGB8888 = bytes B, G, R, X.
            CHECK(px[0] >= 190 && px[0] <= 192);
            CHECK(px[1] >= 127 && px[1] <= 128);
            CHECK(px[2] >= 63 && px[2] <= 64);
        }
    } else {
        std::printf("   output images: shm (device is not the server's render node)\n");
    }
    c->kill_now();
    host.stop();
    std::lock_guard<std::mutex> lock(vk_mutex);
    for (auto& [id, in] : out_imports) imp->destroy(in);
}

}  // namespace

int main() {
    if (private_runtime_dir().empty()) return 1;
    VkHost vkh;
    std::string why;
    if (!vkh.load(&why)) {
        // Nothing was imported: a skip, not a pass.
        std::printf("SKIP: %s\n", why.c_str());
        return 77;
    }
    for (size_t i = 0; i < vkh.devices().size(); ++i) run_device(vkh, i);
    return finish("test_wl_vulkan");
}
