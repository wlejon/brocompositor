#include "win/capture_impl.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <atomic>
#include <cstdio>

namespace brocompositor::win {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

namespace {

std::atomic<uint64_t> g_next_id{1};

struct Slot {
    winrt::com_ptr<ID3D11Texture2D> texture;
    HANDLE handle = nullptr;
    SharedImage desc;
    int leases = 0;
    bool writing = false;
    bool has_frame = false;
    Frame frame;

    ~Slot() {
        if (handle) CloseHandle(handle);
    }
};

}  // namespace

struct WindowCapture::Impl {
    std::shared_ptr<CaptureDevice> device;
    CaptureConfig config;
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frame_revoker;
    wgc::GraphicsCaptureItem::Closed_revoker closed_revoker;

    winrt::com_ptr<ID3D11Fence> fence;
    HANDLE fence_handle = nullptr;
    uint64_t fence_id = g_next_id.fetch_add(1);
    uint64_t fence_value = 0;  // under device context_mutex

    std::mutex frame_mutex;  // serializes on_frame and teardown
    mutable std::mutex mutex;  // slots / latest / leases
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::unique_ptr<Slot>> retiring;
    int latest = -1;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> is_closed{false};
    winrt::Windows::Graphics::SizeInt32 pool_size{0, 0};
    std::function<void()> callback;

    ~Impl() {
        if (fence_handle) CloseHandle(fence_handle);
    }

    bool make_slots(int32_t w, int32_t h, std::string* error);
    void on_frame();
};

bool WindowCapture::Impl::make_slots(int32_t w, int32_t h, std::string* error) {
    // Caller holds `mutex`. Leased images retire until released.
    for (auto& s : slots)
        if (s->leases > 0) retiring.push_back(std::move(s));
    slots.clear();
    latest = -1;
    ++generation;
    auto& dev = device->impl();
    for (uint32_t i = 0; i < std::max(3u, config.ring_size); ++i) {
        auto slot = std::make_unique<Slot>();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = dev.device->CreateTexture2D(&td, nullptr, slot->texture.put());
        if (SUCCEEDED(hr)) {
            hr = slot->texture.as<IDXGIResource1>()->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &slot->handle);
        }
        if (FAILED(hr)) {
            if (error) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "shared texture: 0x%08lx", static_cast<unsigned long>(hr));
                *error = buf;
            }
            return false;
        }
        slot->desc.id = g_next_id.fetch_add(1);
        slot->desc.type = ImageHandleType::D3D11TextureNT;
        slot->desc.handle.value = uint64_t(reinterpret_cast<uintptr_t>(slot->handle));
        slot->desc.width = uint32_t(w);
        slot->desc.height = uint32_t(h);
        slot->desc.format = PixelFormat::BGRA8Unorm;
        slot->desc.adapter = dev.adapter;
        slots.push_back(std::move(slot));
    }
    return true;
}

void WindowCapture::Impl::on_frame() {
    std::lock_guard<std::mutex> serial(frame_mutex);
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    auto content = frame.ContentSize();
    if (content.Width <= 0 || content.Height <= 0) return;
    if (content.Width != pool_size.Width || content.Height != pool_size.Height) {
        // The window was resized: new pool buffers and a new image set.
        pool_size = content;
        pool.Recreate(device->impl().winrt_device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
        std::lock_guard<std::mutex> lock(mutex);
        if (!make_slots(content.Width, content.Height, nullptr)) return;
    }

    winrt::com_ptr<ID3D11Texture2D> src;
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    if (FAILED(access->GetInterface(IID_PPV_ARGS(src.put())))) return;

    Slot* slot = nullptr;
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (int i = 0; i < int(slots.size()); ++i) {
            Slot* s = slots[size_t(i)].get();
            if (i == latest || s->leases > 0 || s->writing) continue;
            if (!slot || (s->frame.sequence < slot->frame.sequence)) {
                slot = s;
                index = i;
            }
        }
        if (!slot) {
            dropped.fetch_add(1);
            return;
        }
        slot->writing = true;
    }

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    uint32_t w = std::min({uint32_t(content.Width), slot->desc.width, sd.Width});
    uint32_t h = std::min({uint32_t(content.Height), slot->desc.height, sd.Height});
    uint64_t value;
    {
        auto& dev = device->impl();
        std::lock_guard<std::mutex> ctx(dev.context_mutex);
        D3D11_BOX box{0, 0, 0, w, h, 1};
        dev.context->CopySubresourceRegion(slot->texture.get(), 0, 0, 0, 0, src.get(), 0, &box);
        value = ++fence_value;
        dev.context->Signal(fence.get(), value);
        dev.context->Flush();
    }

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mutex);
        slot->writing = false;
        slot->has_frame = true;
        Frame& f = slot->frame;
        f.sequence = ++sequence;
        f.image_id = slot->desc.id;
        f.content = Size{int32_t(w), int32_t(h)};
        f.damage.clear();
        f.wait_value = value;
        f.timestamp_ns = int64_t(frame.SystemRelativeTime().count()) * 100;
        latest = index;
        cb = callback;
    }
    if (cb) cb();
}

WindowCapture::WindowCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<WindowCapture> WindowCapture::start(std::shared_ptr<CaptureDevice> device, uint64_t window,
                                                    const CaptureConfig& config, std::string* error) {
    auto fail = [&](std::string msg) -> std::unique_ptr<WindowCapture> {
        if (error) *error = std::move(msg);
        return nullptr;
    };
    if (!device) return fail("no capture device");
    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(window));
    if (!IsWindow(hwnd)) return fail("not a window");

    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->config = config;
    auto& dev = device->impl();
    try {
        HRESULT hr = dev.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence.put()));
        if (FAILED(hr)) return fail("CreateFence(shared) failed");
        hr = impl->fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &impl->fence_handle);
        if (FAILED(hr)) return fail("ID3D11Fence::CreateSharedHandle failed");

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                      winrt::put_abi(impl->item));
        if (FAILED(hr) || !impl->item) return fail("GraphicsCaptureItem::CreateForWindow failed");

        auto size = impl->item.Size();
        if (size.Width <= 0 || size.Height <= 0) return fail("window has no capturable area");
        impl->pool_size = size;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->make_slots(size.Width, size.Height, error)) return nullptr;
        }
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            dev.winrt_device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        Impl* raw = impl.get();
        impl->frame_revoker = impl->pool.FrameArrived(winrt::auto_revoke, [raw](auto&&, auto&&) {
            try {
                raw->on_frame();
            } catch (...) {
                // A frame that cannot be copied is a dropped frame.
                raw->dropped.fetch_add(1);
            }
        });
        impl->closed_revoker =
            impl->item.Closed(winrt::auto_revoke, [raw](auto&&, auto&&) { raw->is_closed.store(true); });
        impl->session = impl->pool.CreateCaptureSession(impl->item);
        try {
            impl->session.IsCursorCaptureEnabled(config.capture_cursor);
        } catch (...) {
        }
        try {
            impl->session.IsBorderRequired(config.show_border);
        } catch (...) {
            // Older systems, or border removal not granted: keep the border.
        }
        impl->session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "WinRT error 0x%08lx", static_cast<unsigned long>(e.code().value));
        return fail(buf);
    }
    return std::unique_ptr<WindowCapture>(new WindowCapture(std::move(impl)));
}

WindowCapture::~WindowCapture() {
    impl_->frame_revoker.revoke();
    impl_->closed_revoker.revoke();
    try {
        if (impl_->session) impl_->session.Close();
        if (impl_->pool) impl_->pool.Close();
    } catch (...) {
    }
    std::lock_guard<std::mutex> serial(impl_->frame_mutex);  // wait out an in-flight frame
}

std::optional<Frame> WindowCapture::acquire() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->latest < 0) return std::nullopt;
    Slot& s = *impl_->slots[size_t(impl_->latest)];
    ++s.leases;
    return s.frame;
}

void WindowCapture::release(const Frame& frame) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& s : impl_->slots) {
        if (s->desc.id == frame.image_id && s->leases > 0) {
            --s->leases;
            return;
        }
    }
    auto& r = impl_->retiring;
    for (auto it = r.begin(); it != r.end(); ++it) {
        if ((*it)->desc.id == frame.image_id && (*it)->leases > 0) {
            if (--(*it)->leases == 0) r.erase(it);
            return;
        }
    }
}

std::vector<SharedImage> WindowCapture::images() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<SharedImage> out;
    for (auto& s : impl_->slots) out.push_back(s->desc);
    return out;
}

std::optional<SharedImage> WindowCapture::image(uint64_t id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& s : impl_->slots)
        if (s->desc.id == id) return s->desc;
    for (auto& s : impl_->retiring)
        if (s->desc.id == id) return s->desc;
    return std::nullopt;
}

uint64_t WindowCapture::images_generation() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->generation;
}

SharedTimeline WindowCapture::timeline() const {
    SharedTimeline t;
    t.type = SyncHandleType::D3D11FenceNT;
    t.handle.value = uint64_t(reinterpret_cast<uintptr_t>(impl_->fence_handle));
    t.id = impl_->fence_id;
    return t;
}

bool WindowCapture::closed() const { return impl_->is_closed.load(); }

void WindowCapture::set_frame_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

uint64_t WindowCapture::frames_dropped() const { return impl_->dropped.load(); }

}  // namespace brocompositor::win
