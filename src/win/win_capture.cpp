#include "win_capture.h"
#include "win_window_ops.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

namespace brocompositor {

WinGraphicsCapture::WinGraphicsCapture() = default;

WinGraphicsCapture::~WinGraphicsCapture() {
    shutdown();
}

bool WinGraphicsCapture::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL feature_level;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &dev,
        &feature_level,
        &ctx
    );

    if (FAILED(hr)) {
        // Fallback to WARP (software rasterizer) if headless / no hardware GPU display
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels,
            ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            &dev,
            &feature_level,
            &ctx
        );
    }

    if (SUCCEEDED(hr) && dev) {
        d3d11_device_ = dev;
        d3d11_context_ = ctx;

        IDXGIDevice* dxgi_dev = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev)))) {
            dxgi_device_ = dxgi_dev;
        }
        initialized_ = true;
        return true;
    }

    return false;
}

void WinGraphicsCapture::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : sessions_) {
        if (pair.second.shared_handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(pair.second.shared_handle));
            pair.second.shared_handle = 0;
        }
        if (pair.second.d3d11_texture) {
            reinterpret_cast<IUnknown*>(pair.second.d3d11_texture)->Release();
            pair.second.d3d11_texture = nullptr;
        }
    }
    sessions_.clear();

    if (dxgi_device_) {
        reinterpret_cast<IUnknown*>(dxgi_device_)->Release();
        dxgi_device_ = nullptr;
    }
    if (d3d11_context_) {
        reinterpret_cast<IUnknown*>(d3d11_context_)->Release();
        d3d11_context_ = nullptr;
    }
    if (d3d11_device_) {
        reinterpret_cast<IUnknown*>(d3d11_device_)->Release();
        d3d11_device_ = nullptr;
    }
    initialized_ = false;
}

bool WinGraphicsCapture::start_capture(WindowId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        if (!initialize()) {
            return false;
        }
    }

    auto it = sessions_.find(id);
    if (it != sessions_.end() && it->second.active) {
        return true;
    }

    WindowInfo win_info;
    if (!WinWindowOps::get_window_info(id, win_info)) {
        return false;
    }

    uint32_t width = (std::max)(1, win_info.geometry.width);
    uint32_t height = (std::max)(1, win_info.geometry.height);

    ID3D11Device* dev = reinterpret_cast<ID3D11Device*>(d3d11_device_);
    if (!dev) return false;

    // Create D3D11 texture with NT Shared Handle support
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        // Fallback to standard legacy shared handle
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = dev->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr)) {
            return false;
        }
    }

    HANDLE shared_handle = nullptr;
    IDXGIResource1* res1 = nullptr;
    if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res1)))) {
        res1->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            &shared_handle
        );
        res1->Release();
    } else {
        IDXGIResource* res = nullptr;
        if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&res)))) {
            res->GetSharedHandle(&shared_handle);
            res->Release();
        }
    }

    Session s;
    s.window_id = id;
    s.width = width;
    s.height = height;
    s.d3d11_texture = texture;
    s.shared_handle = reinterpret_cast<uint64_t>(shared_handle);
    s.frame_index = 1;
    s.active = true;

    sessions_[id] = s;
    return true;
}

void WinGraphicsCapture::stop_capture(WindowId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;

    if (it->second.shared_handle != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(it->second.shared_handle));
    }
    if (it->second.d3d11_texture) {
        reinterpret_cast<IUnknown*>(it->second.d3d11_texture)->Release();
    }
    sessions_.erase(it);
}

bool WinGraphicsCapture::is_capturing(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    return it != sessions_.end() && it->second.active;
}

uint64_t WinGraphicsCapture::get_shared_texture_handle(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    return it != sessions_.end() ? it->second.shared_handle : 0;
}

CaptureFrameInfo WinGraphicsCapture::get_frame_info(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        CaptureFrameInfo info;
        info.window_id = id;
        info.width = it->second.width;
        info.height = it->second.height;
        info.dxgi_format = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
        info.shared_handle = it->second.shared_handle;
        info.frame_index = it->second.frame_index;
        return info;
    }
    return {};
}

} // namespace brocompositor

#else

namespace brocompositor {

WinGraphicsCapture::WinGraphicsCapture() = default;
WinGraphicsCapture::~WinGraphicsCapture() = default;
bool WinGraphicsCapture::initialize() { return true; }
void WinGraphicsCapture::shutdown() {}
bool WinGraphicsCapture::start_capture(WindowId) { return false; }
void WinGraphicsCapture::stop_capture(WindowId) {}
bool WinGraphicsCapture::is_capturing(WindowId) const { return false; }
uint64_t WinGraphicsCapture::get_shared_texture_handle(WindowId) const { return 0; }
CaptureFrameInfo WinGraphicsCapture::get_frame_info(WindowId) const { return {}; }

} // namespace brocompositor

#endif
