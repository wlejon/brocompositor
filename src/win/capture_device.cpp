#include "win/capture_impl.h"

#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <cstring>

namespace brocompositor::win {

namespace {

std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lx", what, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

bool capture_supported() {
    try {
        return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

CaptureDevice::CaptureDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CaptureDevice::~CaptureDevice() = default;

AdapterId CaptureDevice::adapter() const { return impl_->adapter; }
void* CaptureDevice::d3d11_device() const { return impl_->device.get(); }

std::shared_ptr<CaptureDevice> CaptureDevice::create(const CaptureDeviceConfig& config, std::string* error) {
    auto fail = [&](std::string msg) -> std::shared_ptr<CaptureDevice> {
        if (error) *error = std::move(msg);
        return nullptr;
    };
    if (!capture_supported()) return fail("Windows.Graphics.Capture is not supported on this system");

    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (config.adapter) {
        winrt::com_ptr<IDXGIFactory4> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
        if (FAILED(hr)) return fail(hr_text("CreateDXGIFactory1", hr));
        LUID luid;
        std::memcpy(&luid, config.adapter->luid.data(), sizeof(luid));
        hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()));
        if (FAILED(hr)) return fail(hr_text("EnumAdapterByLuid", hr));
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                   device.put(), nullptr, context.put());
    if (FAILED(hr)) return fail(hr_text("D3D11CreateDevice", hr));

    auto impl = std::make_unique<Impl>();
    impl->device = device.try_as<ID3D11Device5>();
    impl->context = context.try_as<ID3D11DeviceContext4>();
    if (!impl->device || !impl->context)
        return fail("ID3D11Device5 / ID3D11DeviceContext4 (shared fences) unavailable");
    if (auto mt = context.try_as<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);

    auto dxgi = device.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> used;
    dxgi->GetAdapter(used.put());
    DXGI_ADAPTER_DESC desc{};
    used->GetDesc(&desc);
    std::memcpy(impl->adapter.luid.data(), &desc.AdapterLuid, sizeof(desc.AdapterLuid));

    winrt::com_ptr<::IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put());
    if (FAILED(hr)) return fail(hr_text("CreateDirect3D11DeviceFromDXGIDevice", hr));
    impl->winrt_device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return std::shared_ptr<CaptureDevice>(new CaptureDevice(std::move(impl)));
}

}  // namespace brocompositor::win
