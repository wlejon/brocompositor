// Windows.Graphics.Capture through the surface contract, consumed the way a
// foreign GPU client would: the shared NT handles are opened on a second,
// independent D3D11 device, the shared fence is waited on GPU-side, and the
// pixels are read back. Covers live updates, parked windows (still captured),
// resize (new image generation), leases and window close.
#include "brocompositor/win/capture.h"

#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <d3d11_4.h>
#include <winrt/base.h>

#include <cstdio>

using namespace brocompositor;
using namespace bctest;

namespace {

struct Consumer {
    winrt::com_ptr<ID3D11Device5> device;
    winrt::com_ptr<ID3D11DeviceContext4> context;
    winrt::com_ptr<ID3D11Fence> fence;

    bool init(const SharedTimeline& t) {
        winrt::com_ptr<ID3D11Device> d;
        winrt::com_ptr<ID3D11DeviceContext> c;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     d.put(), nullptr, c.put())))
            return false;
        device = d.as<ID3D11Device5>();
        context = c.as<ID3D11DeviceContext4>();
        return SUCCEEDED(device->OpenSharedFence(reinterpret_cast<HANDLE>(t.handle.value), IID_PPV_ARGS(fence.put())));
    }

    // GPU-waits for the frame, copies the pixel at (x, y), returns 0xRRGGBB.
    std::optional<uint32_t> pixel(const SharedImage& img, const Frame& f, uint32_t x, uint32_t y) {
        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(device->OpenSharedResource1(reinterpret_cast<HANDLE>(img.handle.value), IID_PPV_ARGS(tex.put()))))
            return std::nullopt;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = 1;
        sd.Height = 1;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = td.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, staging.put()))) return std::nullopt;
        context->Wait(fence.get(), f.wait_value);
        D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return std::nullopt;
        const uint8_t* p = static_cast<const uint8_t*>(m.pData);  // BGRA
        uint32_t rgb = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        context->Unmap(staging.get(), 0);
        return rgb;
    }
};

// Waits for a frame newer than `after` whose centre pixel is `rgb`.
std::optional<Frame> frame_with(win::WindowCapture& cap, Consumer& con, uint64_t after, uint32_t rgb,
                                uint32_t* seen = nullptr) {
    std::optional<Frame> hit;
    wait_until([&] {
        auto f = cap.acquire();
        if (!f) return false;
        bool ok = false;
        if (f->sequence > after) {
            auto img = cap.image(f->image_id);
            if (img) {
                auto px = con.pixel(*img, *f, uint32_t(f->content.width) / 2, uint32_t(f->content.height) / 2);
                if (px && seen) *seen = *px;
                ok = px && *px == rgb;
            }
        }
        if (ok) hit = f;
        cap.release(*f);
        return ok;
    }, 5000ms);
    return hit;
}

void run() {
    if (!win::capture_supported()) {
        std::printf("  Windows.Graphics.Capture unsupported\n");
        bctest::fail(__FILE__, __LINE__, "capture unsupported");
        return;
    }
    TestApp app;
    REQUIRE(app.start());
    Rect wa = primary_work_area();
    HWND h = app.create("cap", Rect{wa.x + 100, wa.y + 100, 640, 480}, "ff0000");
    REQUIRE(h);
    CHECK(wait_until([&] { return IsWindowVisible(h) != FALSE; }));

    std::string err;
    auto device = win::CaptureDevice::create({}, &err);
    if (!device) std::fprintf(stderr, "CaptureDevice: %s\n", err.c_str());
    REQUIRE(device);
    auto cap = win::WindowCapture::start(device, uint64_t(reinterpret_cast<uintptr_t>(h)), {}, &err);
    if (!cap) std::fprintf(stderr, "WindowCapture: %s\n", err.c_str());
    REQUIRE(cap);
    std::atomic<int> callbacks{0};
    cap->set_frame_callback([&] { callbacks.fetch_add(1); });

    auto t = cap->timeline();
    CHECK(t.type == SyncHandleType::D3D11FenceNT && t.handle.value != 0);
    auto imgs = cap->images();
    CHECK_EQ(imgs.size(), size_t(3));
    for (auto& i : imgs) {
        CHECK(i.type == ImageHandleType::D3D11TextureNT && i.handle.value != 0);
        CHECK(i.format == PixelFormat::BGRA8Unorm);
        CHECK(i.adapter == device->adapter());
    }
    Consumer con;
    REQUIRE(con.init(t));

    uint32_t seen = 0;
    auto f1 = frame_with(*cap, con, 0, 0xff0000, &seen);
    if (!f1) std::fprintf(stderr, "  last pixel seen %06x\n", seen);
    REQUIRE(f1.has_value());
    // WGC captures the window's whole DWM frame (outer rect incl. borders).
    std::printf("  first frame: seq %llu content %dx%d (window frame %dx%d)\n",
                (unsigned long long)f1->sequence, f1->content.width, f1->content.height, frame_of(h).width,
                frame_of(h).height);
    CHECK(f1->wait_value > 0);
    CHECK(callbacks.load() > 0);

    // Live update.
    CHECK(app.ok("color cap 00ff00"));
    auto f2 = frame_with(*cap, con, f1->sequence, 0x00ff00);
    CHECK(f2.has_value());
    CHECK(f2 && f2->wait_value > f1->wait_value);

    // A leased image is never handed out for writing: hold one lease while
    // frames keep arriving, and its pixels must stay intact.
    auto held = cap->acquire();
    REQUIRE(held.has_value());
    auto held_img = cap->image(held->image_id);
    for (const char* c : {"0000ff", "ffff00", "00ffff", "ff00ff"}) {
        CHECK(app.ok(std::string("color cap ") + c));
        Sleep(60);
    }
    CHECK_EQ(con.pixel(*held_img, *held, 10 + uint32_t(held->content.width) / 2,
                       uint32_t(held->content.height) / 2),
             std::optional<uint32_t>(0x00ff00));
    cap->release(*held);

    // A parked window (beyond the virtual screen) is not composed by DWM, so
    // WGC delivers no new frames while parked; the last frame stays leasable
    // and capture resumes with current content as soon as it is shown.
    {
        auto shell = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
        REQUIRE(shell);
        WindowId id = kNoWindow;
        wait_until([&] { return (id = shell->find(uint64_t(reinterpret_cast<uintptr_t>(h)))) != kNoWindow; });
        REQUIRE(id != kNoWindow);
        CHECK(shell->set_visible(id, false).get());
        CHECK(!frame_of(h).intersects(virtual_screen_rect()));
        auto last = cap->acquire();
        uint64_t after = last ? last->sequence : 0;
        if (last) cap->release(*last);
        CHECK(app.ok("color cap 123456"));
        Sleep(300);
        auto still = cap->acquire();
        REQUIRE(still.has_value());
        auto still_img = cap->image(still->image_id);
        REQUIRE(still_img.has_value());
        CHECK(con.pixel(*still_img, *still, 20, 20).has_value());  // last frame remains usable
        cap->release(*still);
        uint64_t parked_seq = still->sequence;
        CHECK(shell->set_visible(id, true).get());
        auto back = frame_with(*cap, con, parked_seq, 0x123456);
        CHECK(back.has_value());
        std::printf("  parked: frames %llu -> %llu across park/unpark\n", (unsigned long long)after,
                    back ? (unsigned long long)back->sequence : 0ull);
    }

    // Resize: a new image generation at the new size.
    uint64_t gen = cap->images_generation();
    CHECK(app.ok("move cap " + std::to_string(wa.x + 100) + " " + std::to_string(wa.y + 100) + " 900 600"));
    CHECK(app.ok("color cap 808000"));
    CHECK(frame_with(*cap, con, 0, 0x808000).has_value());
    CHECK(cap->images_generation() > gen);
    // The first frame after a resize may still come from an old-size buffer
    // (content smaller than the image); full-size content follows.
    std::optional<Frame> full;
    CHECK(wait_until([&] {
        app.ok("repaint cap");
        auto f = cap->acquire();
        if (!f) return false;
        auto img = cap->image(f->image_id);
        bool ok = img && uint32_t(f->content.width) == img->width && uint32_t(f->content.height) == img->height;
        if (ok) full = f;
        cap->release(*f);
        return ok;
    }));
    if (full) {
        auto img = cap->image(full->image_id);
        std::printf("  resized: image %ux%u, window frame %dx%d\n", img->width, img->height, frame_of(h).width,
                    frame_of(h).height);
        CHECK_EQ(int32_t(img->width), frame_of(h).width);
        CHECK_EQ(int32_t(img->height), frame_of(h).height);
        CHECK_EQ(con.pixel(*img, *full, img->width / 2, img->height / 2), std::optional<uint32_t>(0x808000));
    }

    // Closing the window closes the source.
    CHECK(app.ok("destroy cap"));
    CHECK(wait_until([&] { return cap->closed(); }));
    std::printf("  frames dropped: %llu\n", (unsigned long long)cap->frames_dropped());
}

}  // namespace

int main() {
    init_windows_test();
    run();
    return finish("test_win_capture");
}
