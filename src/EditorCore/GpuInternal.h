#pragma once

// Solo Windows: i tipi D3D11 dietro GpuDevice e GpuFrameConverter.
#ifdef _WIN32

#include "EditorCore/GpuDevice.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace editor::core {

struct GpuDevice::Impl
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

}  // namespace editor::core

#endif
