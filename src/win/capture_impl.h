#pragma once

#include "brocompositor/win/capture.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <mutex>

namespace brocompositor::win {

struct CaptureDevice::Impl {
    winrt::com_ptr<ID3D11Device5> device;
    winrt::com_ptr<ID3D11DeviceContext4> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device{nullptr};
    AdapterId adapter;
    // Serializes our use of the immediate context across capture callbacks.
    std::mutex context_mutex;
};

}  // namespace brocompositor::win
