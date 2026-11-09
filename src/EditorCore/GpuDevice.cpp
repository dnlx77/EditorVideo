#include "EditorCore/GpuDevice.h"

#ifdef _WIN32

#include "EditorCore/GpuInternal.h"

#include <dxgi.h>

#include <windows.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace editor::core {

namespace {

std::string narrow(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

// Prova a creare il dispositivo con quel tipo di driver; i feature level da 11_1 a 11_0.
bool tryCreate(D3D_DRIVER_TYPE type, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context, D3D_FEATURE_LEVEL& level)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (result == E_INVALIDARG) {  // un sistema senza 11_1 non conosce quel livello nell'elenco
        result = D3D11CreateDevice(nullptr, type, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &device, &level, &context);
    }
    return SUCCEEDED(result);
}

}  // namespace

GpuDevice::GpuDevice() = default;
GpuDevice::~GpuDevice() = default;

std::unique_ptr<GpuDevice> GpuDevice::create(bool allowSoftware, std::string* error)
{
    const auto fail = [&](const char* why) -> std::unique_ptr<GpuDevice> {
        if (error)
            *error = why;
        return nullptr;
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    bool warp = false;
    if (!tryCreate(D3D_DRIVER_TYPE_HARDWARE, device, context, level)) {
        if (!allowSoftware || !tryCreate(D3D_DRIVER_TYPE_WARP, device, context, level))
            return fail("Nessuna scheda video con Direct3D 11 (feature level 11_0).");
        warp = true;
    }
    auto result = std::unique_ptr<GpuDevice>(new GpuDevice());
    result->info_.featureLevel = level == D3D_FEATURE_LEVEL_11_1 ? 111 : 110;
    result->info_.software = warp;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            result->info_.adapterName = narrow(desc.Description);
            // Il «Microsoft Basic Render Driver» (vendor 0x1414) è il rasterizzatore software anche se creato come hardware.
            if (desc.VendorId == 0x1414)
                result->info_.software = true;
        }
    }
    if (result->info_.software && !allowSoftware)
        return fail("C'è solo un rasterizzatore software (nessuna GPU vera): si compone in CPU.");
    if (warp && result->info_.adapterName.empty())
        result->info_.adapterName = "WARP (software)";
    result->impl_ = std::make_unique<Impl>();
    result->impl_->device = std::move(device);
    result->impl_->context = std::move(context);
    return result;
}

}  // namespace editor::core

#else  // altri sistemi: niente GPU, si compone in CPU

namespace editor::core {

struct GpuDevice::Impl
{
};
GpuDevice::GpuDevice() = default;
GpuDevice::~GpuDevice() = default;

std::unique_ptr<GpuDevice> GpuDevice::create(bool, std::string* error)
{
    if (error)
        *error = "Il compositore GPU è solo per Windows.";
    return nullptr;
}

}  // namespace editor::core

#endif
