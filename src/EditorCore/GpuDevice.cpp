#include "EditorCore/GpuDevice.h"

#ifdef _WIN32

#include "EditorCore/GpuInternal.h"

#include <d3d11_4.h>
#include <dxgi.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

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
bool tryCreateWith(UINT flags, D3D_DRIVER_TYPE type, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context, D3D_FEATURE_LEVEL& level)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (result == E_INVALIDARG) {  // un sistema senza 11_1 non conosce quel livello nell'elenco
        result = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &device, &level, &context);
    }
    return SUCCEEDED(result);
}

// Con il supporto video (serve a D3D11VA di FFmpeg, che userà lo stesso dispositivo); se non va, senza.
bool tryCreate(D3D_DRIVER_TYPE type, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context, D3D_FEATURE_LEVEL& level)
{
    return tryCreateWith(D3D11_CREATE_DEVICE_VIDEO_SUPPORT, type, device, context, level) || tryCreateWith(0, type, device, context, level);
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
    // Il dispositivo lo usa anche il decoder (stesso thread, ma FFmpeg e noi insieme): meglio la protezione.
    {
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);
    }
    result->impl_ = std::make_unique<Impl>();
    result->impl_->device = std::move(device);
    result->impl_->context = std::move(context);
    return result;
}

namespace {
// Il dispositivo della texture di un fotogramma D3D11 (nullptr se non lo è).
ComPtr<ID3D11Device> deviceOfFrame(const AVFrame* frame)
{
    ComPtr<ID3D11Device> owner;
    if (frame == nullptr || frame->format != AV_PIX_FMT_D3D11 || frame->data[0] == nullptr)
        return owner;
    reinterpret_cast<ID3D11Texture2D*>(frame->data[0])->GetDevice(&owner);
    return owner;
}

bool sameObject(IUnknown* a, IUnknown* b)
{
    ComPtr<IUnknown> x, y;
    if (a == nullptr || b == nullptr || FAILED(a->QueryInterface(IID_PPV_ARGS(&x))) || FAILED(b->QueryInterface(IID_PPV_ARGS(&y))))
        return false;
    return x.Get() == y.Get();
}
}  // namespace

bool GpuDevice::ownsTextureOf(const AVFrame* frame) const
{
    const ComPtr<ID3D11Device> owner = deviceOfFrame(frame);
    return owner && sameObject(owner.Get(), impl_->device.Get());
}

std::unique_ptr<GpuDevice> GpuDevice::adopt(const AVFrame* frame, std::string* error)
{
    const auto fail = [&](const char* why) -> std::unique_ptr<GpuDevice> {
        if (error)
            *error = why;
        return nullptr;
    };
    ComPtr<ID3D11Device> device = deviceOfFrame(frame);
    if (!device)
        return fail("il fotogramma non è una texture D3D11");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context)
        return fail("contesto del dispositivo del decoder non disponibile");
    auto result = std::unique_ptr<GpuDevice>(new GpuDevice());
    result->info_.featureLevel = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 111 : 110;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
        return fail("il dispositivo del decoder ha un feature level troppo basso");
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            result->info_.adapterName = narrow(desc.Description);
            if (desc.VendorId == 0x1414)
                return fail("il decoder usa un rasterizzatore software");
        }
    }
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
    result->impl_ = std::make_unique<Impl>();
    result->impl_->device = std::move(device);
    result->impl_->context = std::move(context);
    return result;
}

AVBufferRef* GpuDevice::createFfmpegDevice()
{
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (ref == nullptr)
        return nullptr;
    auto* context = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* hardware = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
    hardware->device = impl_->device.Get();
    hardware->device->AddRef();  // FFmpeg lo rilascia quando libera il contesto
    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        return nullptr;
    }
    return ref;
}

}  // namespace editor::core

#else  // altri sistemi: niente GPU, si compone in CPU

namespace editor::core {

struct GpuDevice::Impl
{
};
GpuDevice::GpuDevice() = default;
GpuDevice::~GpuDevice() = default;
AVBufferRef* GpuDevice::createFfmpegDevice() { return nullptr; }
std::unique_ptr<GpuDevice> GpuDevice::adopt(const AVFrame*, std::string* error)
{
    if (error)
        *error = "Il compositore GPU è solo per Windows.";
    return nullptr;
}
bool GpuDevice::ownsTextureOf(const AVFrame*) const { return false; }

std::unique_ptr<GpuDevice> GpuDevice::create(bool, std::string* error)
{
    if (error)
        *error = "Il compositore GPU è solo per Windows.";
    return nullptr;
}

}  // namespace editor::core

#endif
