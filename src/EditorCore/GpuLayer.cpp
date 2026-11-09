#include "EditorCore/GpuColor.h"

#ifdef _WIN32

#include "EditorCore/GpuInternal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace editor::core {

namespace {

// Gli shader dei livelli. Stessa uscita del vertex shader di GpuColor.cpp (SV_Position + TEXCOORD0).
const char* const kLayerShaders = R"hlsl(
cbuffer Layer : register(b1)
{
    float4 rowU;    // u = x*rowU.x + y*rowU.y + rowU.z (posizione nel file, in pixel); rowU.w = lunghezza sullo schermo di un pixel in x
    float4 rowV;    // idem per v
    float4 bounds;  // sinistra, destra, alto, basso della parte visibile (pixel del file)
    float4 limits;  // dove stanno i centri dei pixel leggibili: minX, maxX, minY, maxY
    float4 misc;    // opacità, livello di mip, larghezza e altezza del file
};
Texture2D<float4> layerTex : register(t3);
SamplerState linearSampler : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 psLayer(VSOut i) : SV_Target
{
    float u = rowU.x * i.pos.x + rowU.y * i.pos.y + rowU.z;
    float v = rowV.x * i.pos.x + rowV.y * i.pos.y + rowV.z;
    float coverage = saturate((u - bounds.x) * rowU.w + 0.5) * saturate((bounds.y - u) * rowU.w + 0.5)
                   * saturate((v - bounds.z) * rowV.w + 0.5) * saturate((bounds.w - v) * rowV.w + 0.5);
    if (coverage <= 0.0)
        discard;
    float2 uv = float2(clamp(u, limits.x, limits.y) / misc.z, clamp(v, limits.z, limits.w) / misc.w);
    float3 color = layerTex.SampleLevel(linearSampler, uv, misc.y).rgb;
    return float4(color, coverage * misc.x);
}

float4 psCopy(VSOut i) : SV_Target
{
    return float4(layerTex.Load(int3(int2(i.pos.xy), 0)).rgb, 1.0);
}
)hlsl";

}  // namespace

bool GpuFrameConverter::Impl::ensureLayerResources()
{
    if (layerReady)
        return true;
    const ComPtr<ID3DBlob> layerCode = compileShader(kLayerShaders, "psLayer", "ps_5_0");
    const ComPtr<ID3DBlob> copyCode = compileShader(kLayerShaders, "psCopy", "ps_5_0");
    if (!layerCode || !copyCode)
        return false;
    if (FAILED(device->CreatePixelShader(layerCode->GetBufferPointer(), layerCode->GetBufferSize(), nullptr, &psLayer))
        || FAILED(device->CreatePixelShader(copyCode->GetBufferPointer(), copyCode->GetBufferSize(), nullptr, &psCopy)))
        return false;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 5 * 16;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &layerParams)))
        return false;
    // Colore del livello sopra quello che c'è, con la copertura come alfa; l'alfa del fotogramma non cambia.
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &layerBlend)))
        return false;
    layerReady = true;
    return true;
}

GpuFrameConverter::Impl::SourceTexture* GpuFrameConverter::Impl::uploadSource(const BgraImage& source, bool mips)
{
    if (source.data == nullptr || source.width <= 0 || source.height <= 0 || source.stride < source.width * 4)
        return nullptr;
    // Poche misure alla volta: se ne cambiano troppe si riparte da capo.
    if (sources.size() > 8)
        sources.clear();
    SourceTexture& entry = sources[{source.width, source.height}];
    if (!entry.texture) {
        entry.texture = makeTexture(source.width, source.height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                    D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS);
        if (!entry.texture || FAILED(device->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.view))) {
            sources.erase({source.width, source.height});
            return nullptr;
        }
    }
    context->UpdateSubresource(entry.texture.Get(), 0, nullptr, source.data, static_cast<UINT>(source.stride), 0);
    if (mips)
        context->GenerateMips(entry.view.Get());
    return &entry;
}

bool GpuFrameConverter::clearCanvas(float r, float g, float b)
{
    Impl& d = *impl_;
    const float color[4] = {r, g, b, 1.0f};
    d.context->ClearRenderTargetView(d.rgbTarget.Get(), color);
    return true;
}

bool GpuFrameConverter::setCanvasBgra(const BgraImage& image)
{
    Impl& d = *impl_;
    if (image.width != d.width || image.height != d.height || !d.ensureLayerResources())
        return false;
    const Impl::SourceTexture* texture = d.uploadSource(image, false);
    if (texture == nullptr)
        return false;
    ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, nullptr, texture->view.Get()};
    d.pass(d.rgbTarget.Get(), 0, 0, d.width, d.height, d.psCopy.Get(), views, 4);
    return true;
}

bool GpuFrameConverter::drawLayer(const BgraImage& source, const Transform& transform)
{
    Impl& d = *impl_;
    if (source.data == nullptr || source.width <= 0 || source.height <= 0)
        return false;
    const Transform t = normalized(transform);
    if (t.opacity <= 0.0)
        return true;  // niente da disegnare
    if (!d.ensureLayerResources())
        return false;

    const TransformGeometry g = computeGeometry(t, source.width, source.height, d.width, d.height);
    const PixelBounds box = pixelBounds(g.corners, d.width, d.height);
    if (box.empty())
        return true;
    const Affine inv = g.toProject.inverse();
    const float lenX = static_cast<float>(std::hypot(g.toProject.a, g.toProject.b));
    const float lenY = static_cast<float>(std::hypot(g.toProject.c, g.toProject.d));
    // Quanto si rimpicciolisce il file: da qui il livello di mip (rimpicciolito 4 volte = livello 2).
    const double scale = std::sqrt(std::abs(g.toProject.a * g.toProject.d - g.toProject.b * g.toProject.c));
    const float lod = scale > 0.0 && scale < 1.0 ? static_cast<float>(std::log2(1.0 / scale)) : 0.0f;
    // I pixel leggibili restano dentro la parte visibile (così non entra il colore di quello ritagliato).
    const int minIx = std::clamp(static_cast<int>(std::floor(g.sourceLeft)), 0, source.width - 1);
    const int maxIx = std::clamp(static_cast<int>(std::ceil(g.sourceRight)) - 1, minIx, source.width - 1);
    const int minIy = std::clamp(static_cast<int>(std::floor(g.sourceTop)), 0, source.height - 1);
    const int maxIy = std::clamp(static_cast<int>(std::ceil(g.sourceBottom)) - 1, minIy, source.height - 1);

    const float values[20] = {
        static_cast<float>(inv.a), static_cast<float>(inv.c), static_cast<float>(inv.tx), lenX,
        static_cast<float>(inv.b), static_cast<float>(inv.d), static_cast<float>(inv.ty), lenY,
        static_cast<float>(g.sourceLeft), static_cast<float>(g.sourceRight), static_cast<float>(g.sourceTop), static_cast<float>(g.sourceBottom),
        static_cast<float>(minIx) + 0.5f, static_cast<float>(maxIx) + 0.5f, static_cast<float>(minIy) + 0.5f, static_cast<float>(maxIy) + 0.5f,
        static_cast<float>(t.opacity), lod, static_cast<float>(source.width), static_cast<float>(source.height),
    };
    const Impl::SourceTexture* texture = d.uploadSource(source, true);
    if (texture == nullptr)
        return false;
    d.context->UpdateSubresource(d.layerParams.Get(), 0, nullptr, values, 0, 0);
    ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, nullptr, texture->view.Get()};
    d.pass(d.rgbTarget.Get(), box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0, d.psLayer.Get(), views, 4, d.layerBlend.Get());
    return true;
}

}  // namespace editor::core

#else  // altri sistemi

namespace editor::core {

bool GpuFrameConverter::clearCanvas(float, float, float) { return false; }
bool GpuFrameConverter::setCanvasBgra(const BgraImage&) { return false; }
bool GpuFrameConverter::drawLayer(const BgraImage&, const Transform&) { return false; }

}  // namespace editor::core

#endif
