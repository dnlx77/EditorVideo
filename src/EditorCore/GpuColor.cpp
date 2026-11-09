#include "EditorCore/GpuColor.h"

#ifdef _WIN32

#include "EditorCore/GpuInternal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace editor::core {

namespace {

// Gli shader: un triangolo che copre tutto lo schermo e i passaggi NV12 -> RGBA16F e RGBA16F -> Y e UV.
// Le formule sono quelle di swscale con i coefficienti (kr, kb) e la gamma (limitata 16-235 / 224, o piena).
const char* const kShaders = R"hlsl(
cbuffer Params : register(b0) { float4 p; }  // x = kr, y = kb, z = 1 se gamma piena
Texture2D<float> yTex : register(t0);
Texture2D<float2> uvTex : register(t1);
Texture2D<float4> rgbTex : register(t2);
SamplerState linearSampler : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 psNv12ToRgb(VSOut i) : SV_Target
{
    float y = yTex.Load(int3(int2(i.pos.xy), 0));
    float2 c = uvTex.SampleLevel(linearSampler, i.uv, 0);
    float kr = p.x, kb = p.y, kg = 1.0 - p.x - p.y;
    if (p.z > 0.5) {
        c -= 128.0 / 255.0;
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        c = (c - 128.0 / 255.0) * (255.0 / 224.0);
    }
    float r = y + 2.0 * (1.0 - kr) * c.y;
    float b = y + 2.0 * (1.0 - kb) * c.x;
    float g = (y - kr * r - kb * b) / kg;
    return float4(saturate(float3(r, g, b)), 1.0);
}

float psRgbToY(VSOut i) : SV_Target
{
    float3 c = rgbTex.Load(int3(int2(i.pos.xy), 0)).rgb;
    float y = p.x * c.r + (1.0 - p.x - p.y) * c.g + p.y * c.b;
    return p.z > 0.5 ? y : 16.0 / 255.0 + y * (219.0 / 255.0);
}

float2 psRgbToUV(VSOut i) : SV_Target
{
    uint w, h;
    rgbTex.GetDimensions(w, h);
    int2 xy = int2(i.pos.xy) * 2;
    int2 last = int2(w - 1, h - 1);
    float3 c = rgbTex.Load(int3(min(xy, last), 0)).rgb;
    c += rgbTex.Load(int3(min(xy + int2(1, 0), last), 0)).rgb;
    c += rgbTex.Load(int3(min(xy + int2(0, 1), last), 0)).rgb;
    c += rgbTex.Load(int3(min(xy + int2(1, 1), last), 0)).rgb;
    c *= 0.25;
    float kr = p.x, kb = p.y;
    float y = kr * c.r + (1.0 - kr - kb) * c.g + kb * c.b;
    float cb = (c.b - y) / (2.0 * (1.0 - kb));
    float cr = (c.r - y) / (2.0 * (1.0 - kr));
    if (p.z < 0.5) {
        cb *= 224.0 / 255.0;
        cr *= 224.0 / 255.0;
    }
    return float2(cb, cr) + 128.0 / 255.0;
}
)hlsl";

ComPtr<ID3DBlob> compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT result =
        D3DCompile(kShaders, std::strlen(kShaders), "compositor.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    return SUCCEEDED(result) ? code : nullptr;
}

}  // namespace

struct GpuFrameConverter::Impl
{
    GpuDevice* gpu = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    int width = 0;
    int height = 0;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psToRgb, psToY, psToUV;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;

    ComPtr<ID3D11Texture2D> yTex, uvTex, rgbTex, yOut, uvOut;
    ComPtr<ID3D11ShaderResourceView> yView, uvView, rgbView;
    ComPtr<ID3D11RenderTargetView> rgbTarget, yTarget, uvTarget;
    ComPtr<ID3D11Texture2D> yStage, uvStage, rgbStage;

    ComPtr<ID3D11Texture2D> makeTexture(int w, int h, DXGI_FORMAT format, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = cpu;
        ComPtr<ID3D11Texture2D> texture;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) ? texture : nullptr;
    }

    bool init()
    {
        const ComPtr<ID3DBlob> vsCode = compile("vs", "vs_5_0");
        const ComPtr<ID3DBlob> toRgbCode = compile("psNv12ToRgb", "ps_5_0");
        const ComPtr<ID3DBlob> toYCode = compile("psRgbToY", "ps_5_0");
        const ComPtr<ID3DBlob> toUVCode = compile("psRgbToUV", "ps_5_0");
        if (!vsCode || !toRgbCode || !toYCode || !toUVCode)
            return false;
        if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs))
            || FAILED(device->CreatePixelShader(toRgbCode->GetBufferPointer(), toRgbCode->GetBufferSize(), nullptr, &psToRgb))
            || FAILED(device->CreatePixelShader(toYCode->GetBufferPointer(), toYCode->GetBufferSize(), nullptr, &psToY))
            || FAILED(device->CreatePixelShader(toUVCode->GetBufferPointer(), toUVCode->GetBufferSize(), nullptr, &psToUV)))
            return false;

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = 16;
        bufferDesc.Usage = D3D11_USAGE_DEFAULT;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &params)))
            return false;
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&samplerDesc, &sampler)))
            return false;
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterDesc, &raster)))
            return false;

        const int cw = width / 2;
        const int ch = height / 2;
        yTex = makeTexture(width, height, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        uvTex = makeTexture(cw, ch, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        rgbTex = makeTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        yOut = makeTexture(width, height, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET);
        uvOut = makeTexture(cw, ch, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_RENDER_TARGET);
        yStage = makeTexture(width, height, DXGI_FORMAT_R8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        uvStage = makeTexture(cw, ch, DXGI_FORMAT_R8G8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        rgbStage = makeTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        if (!yTex || !uvTex || !rgbTex || !yOut || !uvOut || !yStage || !uvStage || !rgbStage)
            return false;
        return SUCCEEDED(device->CreateShaderResourceView(yTex.Get(), nullptr, &yView))
            && SUCCEEDED(device->CreateShaderResourceView(uvTex.Get(), nullptr, &uvView))
            && SUCCEEDED(device->CreateShaderResourceView(rgbTex.Get(), nullptr, &rgbView))
            && SUCCEEDED(device->CreateRenderTargetView(rgbTex.Get(), nullptr, &rgbTarget))
            && SUCCEEDED(device->CreateRenderTargetView(yOut.Get(), nullptr, &yTarget))
            && SUCCEEDED(device->CreateRenderTargetView(uvOut.Get(), nullptr, &uvTarget));
    }

    void setParams(const GpuColorSpace& colors)
    {
        const float values[4] = {static_cast<float>(colors.kr), static_cast<float>(colors.kb), colors.fullRange ? 1.0f : 0.0f, 0.0f};
        context->UpdateSubresource(params.Get(), 0, nullptr, values, 0, 0);
    }

    // Un passaggio a tutto schermo su `target` (di `w` x `h`) con `shader`; `views` sono i t0.. dello shader.
    void pass(ID3D11RenderTargetView* target, int w, int h, ID3D11PixelShader* shader, ID3D11ShaderResourceView* const* views, UINT viewCount)
    {
        D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &viewport);
        context->RSSetState(raster.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(shader, nullptr, 0);
        ID3D11Buffer* constants = params.Get();
        context->PSSetConstantBuffers(0, 1, &constants);
        ID3D11SamplerState* samplers = sampler.Get();
        context->PSSetSamplers(0, 1, &samplers);
        context->PSSetShaderResources(0, viewCount, views);
        context->Draw(3, 0);
        // Si sciolgono i legami: la stessa texture può essere subito dopo scritta e letta.
        ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
        context->PSSetShaderResources(0, 3, none);
        ID3D11RenderTargetView* noTarget = nullptr;
        context->OMSetRenderTargets(1, &noTarget, nullptr);
    }

    // Copia `rows` righe di `bytesPerRow` byte della texture di prova `stage` (già riempita) in `dst`.
    bool readStage(ID3D11Texture2D* stage, std::uint8_t* dst, int dstStride, int rows, int bytesPerRow)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(stage, 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
        for (int y = 0; y < rows; ++y)
            std::memcpy(dst + static_cast<std::ptrdiff_t>(y) * dstStride, src + static_cast<std::size_t>(y) * mapped.RowPitch,
                        static_cast<std::size_t>(bytesPerRow));
        context->Unmap(stage, 0);
        return true;
    }
};

GpuFrameConverter::GpuFrameConverter() = default;
GpuFrameConverter::~GpuFrameConverter() = default;

std::unique_ptr<GpuFrameConverter> GpuFrameConverter::create(GpuDevice& device, int width, int height)
{
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0)
        return nullptr;
    auto converter = std::unique_ptr<GpuFrameConverter>(new GpuFrameConverter());
    converter->impl_ = std::make_unique<Impl>();
    Impl& impl = *converter->impl_;
    impl.gpu = &device;
    impl.device = device.impl().device.Get();
    impl.context = device.impl().context.Get();
    impl.width = width;
    impl.height = height;
    if (!impl.init())
        return nullptr;
    return converter;
}

int GpuFrameConverter::width() const { return impl_->width; }
int GpuFrameConverter::height() const { return impl_->height; }

bool GpuFrameConverter::uploadNv12(const AVFrame* nv12, const GpuColorSpace& colors)
{
    Impl& d = *impl_;
    if (nv12 == nullptr || nv12->format != AV_PIX_FMT_NV12 || nv12->width != d.width || nv12->height != d.height || nv12->data[0] == nullptr
        || nv12->data[1] == nullptr || nv12->linesize[0] <= 0 || nv12->linesize[1] <= 0)
        return false;
    d.context->UpdateSubresource(d.yTex.Get(), 0, nullptr, nv12->data[0], static_cast<UINT>(nv12->linesize[0]), 0);
    d.context->UpdateSubresource(d.uvTex.Get(), 0, nullptr, nv12->data[1], static_cast<UINT>(nv12->linesize[1]), 0);
    d.setParams(colors);
    ID3D11ShaderResourceView* views[2] = {d.yView.Get(), d.uvView.Get()};
    d.pass(d.rgbTarget.Get(), d.width, d.height, d.psToRgb.Get(), views, 2);
    return true;
}

bool GpuFrameConverter::downloadNv12(AVFrame* nv12, const GpuColorSpace& colors)
{
    Impl& d = *impl_;
    if (nv12 == nullptr || nv12->format != AV_PIX_FMT_NV12 || nv12->width != d.width || nv12->height != d.height || nv12->data[0] == nullptr
        || nv12->data[1] == nullptr || nv12->linesize[0] <= 0 || nv12->linesize[1] <= 0)
        return false;
    d.setParams(colors);
    // Il passaggio vuole la texture RGBA al posto t2 (come nello shader): si mettono t0 e t1 vuoti.
    ID3D11ShaderResourceView* views[3] = {nullptr, nullptr, d.rgbView.Get()};
    d.pass(d.yTarget.Get(), d.width, d.height, d.psToY.Get(), views, 3);
    d.pass(d.uvTarget.Get(), d.width / 2, d.height / 2, d.psToUV.Get(), views, 3);
    d.context->CopyResource(d.yStage.Get(), d.yOut.Get());
    d.context->CopyResource(d.uvStage.Get(), d.uvOut.Get());
    return d.readStage(d.yStage.Get(), nv12->data[0], nv12->linesize[0], d.height, d.width)
        && d.readStage(d.uvStage.Get(), nv12->data[1], nv12->linesize[1], d.height / 2, d.width);
}

bool GpuFrameConverter::readRgbaHalf(std::vector<std::uint16_t>& out)
{
    Impl& d = *impl_;
    d.context->CopyResource(d.rgbStage.Get(), d.rgbTex.Get());
    out.resize(static_cast<std::size_t>(d.width) * d.height * 4);
    return d.readStage(d.rgbStage.Get(), reinterpret_cast<std::uint8_t*>(out.data()), d.width * 8, d.height, d.width * 8);
}

}  // namespace editor::core

#else  // altri sistemi

namespace editor::core {

struct GpuFrameConverter::Impl
{
};
GpuFrameConverter::GpuFrameConverter() = default;
GpuFrameConverter::~GpuFrameConverter() = default;
std::unique_ptr<GpuFrameConverter> GpuFrameConverter::create(GpuDevice&, int, int) { return nullptr; }
int GpuFrameConverter::width() const { return 0; }
int GpuFrameConverter::height() const { return 0; }
bool GpuFrameConverter::uploadNv12(const AVFrame*, const GpuColorSpace&) { return false; }
bool GpuFrameConverter::downloadNv12(AVFrame*, const GpuColorSpace&) { return false; }
bool GpuFrameConverter::readRgbaHalf(std::vector<std::uint16_t>&) { return false; }

}  // namespace editor::core

#endif
