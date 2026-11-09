#include "EditorCore/GpuColor.h"

#ifdef _WIN32

#include "EditorCore/GpuInternal.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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
Texture2D<float4> fromTex : register(t2);  // transizioni: il fotogramma che finisce (layerTex è quello che inizia)
SamplerState linearSampler : register(s0);
SamplerState borderSampler : register(s1);

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

// Una scritta (colori premoltiplicati): rowU/rowV portano la posizione del pixel nella scritta (in pixel), misc.x l'opacità.
float4 psText(VSOut i) : SV_Target
{
    float u = rowU.x * i.pos.x + rowU.y * i.pos.y + rowU.z;
    float v = rowV.x * i.pos.x + rowV.y * i.pos.y + rowV.z;
    return layerTex.SampleLevel(borderSampler, float2(u / misc.z, v / misc.w), 0) * misc.x;
}

// Transizioni. bounds = (peso di «da», peso di «a», spostamento in pixel, direzione 0 sinistra 1 destra 2 su 3 giù), misc.x = effetto
// (0 dissolvenza incrociata, 1 dissolvenza al nero: solo i pesi; 2 scorrimento), misc.zw = misura.
float4 psTransition(VSOut i) : SV_Target
{
    int2 xy = int2(i.pos.xy);
    if (misc.x < 1.5) {
        float3 a = fromTex.Load(int3(xy, 0)).rgb;
        float3 b = layerTex.Load(int3(xy, 0)).rgb;
        return float4(min(a * bounds.x + b * bounds.y, 1.0), 1.0);
    }
    int dir = (int)(bounds.w + 0.5);
    bool horizontal = dir < 2;
    bool forward = dir == 0 || dir == 2;
    int size = (int)(horizontal ? misc.z : misc.w);
    int shift = (int)(bounds.z + 0.5);
    int c = horizontal ? xy.x : xy.y;
    int pos = forward ? c : size - 1 - c;
    bool fromSide = pos < size - shift;
    int sx = fromSide ? pos + shift : pos - (size - shift);
    int src = forward ? sx : size - 1 - sx;
    int2 at = horizontal ? int2(src, xy.y) : int2(xy.x, src);
    float3 color = fromSide ? fromTex.Load(int3(at, 0)).rgb : layerTex.Load(int3(at, 0)).rgb;
    return float4(color, 1.0);
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
    const ComPtr<ID3DBlob> textCode = compileShader(kLayerShaders, "psText", "ps_5_0");
    const ComPtr<ID3DBlob> transitionCode = compileShader(kLayerShaders, "psTransition", "ps_5_0");
    if (!layerCode || !copyCode || !textCode || !transitionCode)
        return false;
    if (FAILED(device->CreatePixelShader(layerCode->GetBufferPointer(), layerCode->GetBufferSize(), nullptr, &psLayer))
        || FAILED(device->CreatePixelShader(copyCode->GetBufferPointer(), copyCode->GetBufferSize(), nullptr, &psCopy))
        || FAILED(device->CreatePixelShader(textCode->GetBufferPointer(), textCode->GetBufferSize(), nullptr, &psText))
        || FAILED(device->CreatePixelShader(transitionCode->GetBufferPointer(), transitionCode->GetBufferSize(), nullptr, &psTransition)))
        return false;
    scratch = makeTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!scratch || FAILED(device->CreateRenderTargetView(scratch.Get(), nullptr, &scratchTarget)))
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
    // La scritta è premoltiplicata: colore + quello che c'è sotto per (1 - alfa).
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    if (FAILED(device->CreateBlendState(&blendDesc, &textBlend)))
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
    if (normalized(transform).opacity <= 0.0)
        return true;  // niente da disegnare
    if (!d.ensureLayerResources())
        return false;
    const Impl::SourceTexture* texture = d.uploadSource(source, true);
    if (texture == nullptr)
        return false;
    return d.drawLayerView(texture->view.Get(), source.width, source.height, transform);
}

bool GpuFrameConverter::drawNv12(const AVFrame* nv12, const GpuColorSpace& colors, const Transform& transform)
{
    Impl& d = *impl_;
    if (nv12 == nullptr || nv12->format != AV_PIX_FMT_NV12 || nv12->width <= 0 || nv12->height <= 0 || nv12->data[0] == nullptr
        || nv12->data[1] == nullptr || nv12->linesize[0] <= 0 || nv12->linesize[1] <= 0)
        return false;
    const Transform t = normalized(transform);
    if (t.opacity <= 0.0)
        return true;
    if (!d.ensureLayerResources())
        return false;
    const int w = nv12->width;
    const int h = nv12->height;
    Impl::Nv12Source* found = d.nv12Source(w, h);
    if (found == nullptr)
        return false;
    Impl::Nv12Source& source = *found;
    d.context->UpdateSubresource(source.y.Get(), 0, nullptr, nv12->data[0], static_cast<UINT>(nv12->linesize[0]), 0);
    d.context->UpdateSubresource(source.uv.Get(), 0, nullptr, nv12->data[1], static_cast<UINT>(nv12->linesize[1]), 0);
    d.setParams(colors);
    ID3D11ShaderResourceView* views[2] = {source.yView.Get(), source.uvView.Get()};
    d.pass(source.rgbTarget.Get(), 0, 0, w, h, d.psToRgb.Get(), views, 2);
    d.context->GenerateMips(source.rgbView.Get());
    return d.drawLayerView(source.rgbView.Get(), w, h, t);
}

GpuFrameConverter::Impl::Nv12Source* GpuFrameConverter::Impl::nv12Source(int w, int h)
{
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    if (nv12Sources.size() > 4)
        nv12Sources.clear();
    Nv12Source& source = nv12Sources[{w, h}];
    if (!source.y) {
        source.y = makeTexture(w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        source.uv = makeTexture(cw, ch, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        source.rgb = makeTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                 D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS);
        if (!source.y || !source.uv || !source.rgb || FAILED(device->CreateShaderResourceView(source.y.Get(), nullptr, &source.yView))
            || FAILED(device->CreateShaderResourceView(source.uv.Get(), nullptr, &source.uvView))
            || FAILED(device->CreateShaderResourceView(source.rgb.Get(), nullptr, &source.rgbView))
            || FAILED(device->CreateRenderTargetView(source.rgb.Get(), nullptr, &source.rgbTarget))) {
            nv12Sources.erase({w, h});
            return nullptr;
        }
    }
    return &source;
}

bool GpuFrameConverter::drawD3d11Frame(const AVFrame* frame, const GpuColorSpace& colors, const Transform& transform, std::string* why)
{
    const auto no = [&](const std::string& reason) {
        if (why != nullptr)
            *why = reason;
        return false;
    };
    Impl& d = *impl_;
    if (frame == nullptr || frame->format != AV_PIX_FMT_D3D11 || frame->hw_frames_ctx == nullptr || frame->data[0] == nullptr
        || frame->width <= 0 || frame->height <= 0 || (frame->width & 1) != 0 || (frame->height & 1) != 0)
        return no("fotogramma non adatto (formato o misura dispari)");
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (frames->sw_format != AV_PIX_FMT_NV12)
        return no(std::string("formato dei fotogrammi del decoder non NV12 (") + av_get_pix_fmt_name(frames->sw_format) + ")");
    auto* decoded = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame->data[1]));
    ComPtr<ID3D11Device> owner;
    decoded->GetDevice(&owner);
    if (owner.Get() != d.device)
        return no("il decoder usa un altro dispositivo D3D11");
    D3D11_TEXTURE2D_DESC desc{};
    decoded->GetDesc(&desc);
    const int w = frame->width;
    const int h = frame->height;
    if (desc.Format != DXGI_FORMAT_NV12 || desc.Width < static_cast<UINT>(w) || desc.Height < static_cast<UINT>(h) || slice >= desc.ArraySize)
        return no("texture del decoder inattesa (formato " + std::to_string(desc.Format) + ", " + std::to_string(desc.Width) + "x"
                  + std::to_string(desc.Height) + ", indice " + std::to_string(slice) + "/" + std::to_string(desc.ArraySize) + ")");
    const Transform t = normalized(transform);
    if (t.opacity <= 0.0)
        return true;
    if (!d.ensureLayerResources())
        return no("risorse dei livelli non create");
    Impl::Nv12Source* found = d.nv12Source(w, h);
    if (found == nullptr)
        return no("texture di conversione non create");
    Impl::Nv12Source& source = *found;
    if (!source.packed) {
        if (source.packedFailed)
            return no("texture NV12 non creabile su questa scheda");
        source.packedFailed = true;  // si toglie solo se tutto va bene: se questa scheda non lo sa fare non si riprova a ogni fotogramma
        D3D11_TEXTURE2D_DESC packed{};
        packed.Width = static_cast<UINT>(w);
        packed.Height = static_cast<UINT>(h);
        packed.MipLevels = 1;
        packed.ArraySize = 1;
        packed.Format = DXGI_FORMAT_NV12;
        packed.SampleDesc.Count = 1;
        packed.Usage = D3D11_USAGE_DEFAULT;
        packed.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> yView, uvView;
        HRESULT result = d.device->CreateTexture2D(&packed, nullptr, &texture);
        if (FAILED(result))
            return no("texture NV12 non creabile (errore " + std::to_string(static_cast<unsigned long>(result)) + ")");
        view.Format = DXGI_FORMAT_R8_UNORM;
        result = d.device->CreateShaderResourceView(texture.Get(), &view, &yView);
        if (FAILED(result))
            return no("vista del piano Y non creabile (errore " + std::to_string(static_cast<unsigned long>(result)) + ")");
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        result = d.device->CreateShaderResourceView(texture.Get(), &view, &uvView);
        if (FAILED(result))
            return no("vista dei piani UV non creabile (errore " + std::to_string(static_cast<unsigned long>(result)) + ")");
        source.packed = texture;
        source.packedY = yView;
        source.packedUV = uvView;
        source.packedFailed = false;
    }
    const D3D11_BOX box{0, 0, 0, static_cast<UINT>(w), static_cast<UINT>(h), 1};
    d.context->CopySubresourceRegion(source.packed.Get(), 0, 0, 0, 0, decoded, D3D11CalcSubresource(0, slice, desc.MipLevels), &box);
    d.setParams(colors);
    ID3D11ShaderResourceView* views[2] = {source.packedY.Get(), source.packedUV.Get()};
    d.pass(source.rgbTarget.Get(), 0, 0, w, h, d.psToRgb.Get(), views, 2);
    d.context->GenerateMips(source.rgbView.Get());
    return d.drawLayerView(source.rgbView.Get(), w, h, t);
}

bool GpuFrameConverter::Impl::drawLayerView(ID3D11ShaderResourceView* view, int sourceWidth, int sourceHeight, const Transform& transform)
{
    const Transform t = normalized(transform);
    if (t.opacity <= 0.0)
        return true;
    const TransformGeometry g = computeGeometry(t, sourceWidth, sourceHeight, width, height);
    const PixelBounds box = pixelBounds(g.corners, width, height);
    if (box.empty())
        return true;
    const Affine inv = g.toProject.inverse();
    const float lenX = static_cast<float>(std::hypot(g.toProject.a, g.toProject.b));
    const float lenY = static_cast<float>(std::hypot(g.toProject.c, g.toProject.d));
    // Quanto si rimpicciolisce il file: da qui il livello di mip (rimpicciolito 4 volte = livello 2).
    const double scale = std::sqrt(std::abs(g.toProject.a * g.toProject.d - g.toProject.b * g.toProject.c));
    const float lod = scale > 0.0 && scale < 1.0 ? static_cast<float>(std::log2(1.0 / scale)) : 0.0f;
    // I pixel leggibili restano dentro la parte visibile (così non entra il colore di quello ritagliato).
    const int minIx = std::clamp(static_cast<int>(std::floor(g.sourceLeft)), 0, sourceWidth - 1);
    const int maxIx = std::clamp(static_cast<int>(std::ceil(g.sourceRight)) - 1, minIx, sourceWidth - 1);
    const int minIy = std::clamp(static_cast<int>(std::floor(g.sourceTop)), 0, sourceHeight - 1);
    const int maxIy = std::clamp(static_cast<int>(std::ceil(g.sourceBottom)) - 1, minIy, sourceHeight - 1);

    const float values[20] = {
        static_cast<float>(inv.a), static_cast<float>(inv.c), static_cast<float>(inv.tx), lenX,
        static_cast<float>(inv.b), static_cast<float>(inv.d), static_cast<float>(inv.ty), lenY,
        static_cast<float>(g.sourceLeft), static_cast<float>(g.sourceRight), static_cast<float>(g.sourceTop), static_cast<float>(g.sourceBottom),
        static_cast<float>(minIx) + 0.5f, static_cast<float>(maxIx) + 0.5f, static_cast<float>(minIy) + 0.5f, static_cast<float>(maxIy) + 0.5f,
        static_cast<float>(t.opacity), lod, static_cast<float>(sourceWidth), static_cast<float>(sourceHeight),
    };
    context->UpdateSubresource(layerParams.Get(), 0, nullptr, values, 0, 0);
    ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, nullptr, view};
    pass(rgbTarget.Get(), box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0, psLayer.Get(), views, 4, layerBlend.Get());
    return true;
}

bool GpuFrameConverter::supportsTransition(const std::string& transitionId)
{
    return transitionId == "dissolvenza" || transitionId == "dissolvenza-nero" || transitionId == "scorrimento";
}

bool GpuFrameConverter::blendTransition(GpuFrameConverter& to, const std::string& transitionId, double progress, const TransitionParams& params)
{
    Impl& d = *impl_;
    Impl& other = *to.impl_;
    if (other.width != d.width || other.height != d.height || other.device != d.device || !d.ensureLayerResources())
        return false;
    const double p = std::clamp(progress, 0.0, 1.0);
    float weightFrom = 0.0f;
    float weightTo = 0.0f;
    float shift = 0.0f;
    float effect = 0.0f;
    if (transitionId == "dissolvenza") {
        weightFrom = static_cast<float>(1.0 - p);
        weightTo = static_cast<float>(p);
    } else if (transitionId == "dissolvenza-nero") {
        effect = 1.0f;
        weightFrom = p < 0.5 ? static_cast<float>(1.0 - 2.0 * p) : 0.0f;
        weightTo = p < 0.5 ? 0.0f : static_cast<float>(2.0 * p - 1.0);
    } else if (transitionId == "scorrimento") {
        effect = 2.0f;
        const bool horizontal = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Right;
        const int size = horizontal ? d.width : d.height;
        shift = static_cast<float>(std::clamp(static_cast<int>(std::lround(p * size)), 0, size));
    } else {
        return false;
    }
    const float values[20] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        weightFrom, weightTo, shift, static_cast<float>(static_cast<int>(params.direction)),
        0, 0, 0, 0,
        effect, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height),
    };
    d.context->UpdateSubresource(d.layerParams.Get(), 0, nullptr, values, 0, 0);
    ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, d.rgbView.Get(), other.rgbView.Get()};
    d.pass(d.scratchTarget.Get(), 0, 0, d.width, d.height, d.psTransition.Get(), views, 4);
    d.context->CopyResource(d.rgbTex.Get(), d.scratch.Get());
    return true;
}

std::shared_ptr<GpuTextImage> GpuFrameConverter::createText(const TextImage& image)
{
    Impl& d = *impl_;
    if (image.empty() || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4)
        return nullptr;
    auto text = std::make_shared<GpuTextImage>();
    text->x = image.x;
    text->y = image.y;
    text->width = image.width;
    text->height = image.height;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(image.width);
    desc.Height = static_cast<UINT>(image.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = image.rgba.data();
    data.SysMemPitch = static_cast<UINT>(image.width) * 4;
    if (FAILED(d.device->CreateTexture2D(&desc, &data, &text->texture)) || FAILED(d.device->CreateShaderResourceView(text->texture.Get(), nullptr, &text->view)))
        return nullptr;
    return text;
}

bool GpuFrameConverter::drawText(const GpuTextImage& text, const Transform& transform, double pivotX, double pivotY, double opacity)
{
    Impl& d = *impl_;
    const double op = std::clamp(opacity, 0.0, 1.0);
    if (op <= 0.0)
        return true;
    if (!d.ensureLayerResources())
        return false;
    // Dal pixel del fotogramma al pixel della scritta: la stessa geometria di transformTextImage.
    const Transform t = normalized(transform);
    Affine forward;  // identità: la scritta sta dov'è
    if (t.movesPicture()) {
        forward = linearPart(t);
        forward.tx = pivotX + t.posX - (forward.a * pivotX + forward.c * pivotY);
        forward.ty = pivotY + t.posY - (forward.b * pivotX + forward.d * pivotY);
    }
    const Affine inverse = forward.inverse();
    const double left = text.x;
    const double top = text.y;
    const double right = text.x + text.width;
    const double bottom = text.y + text.height;
    const PixelBounds box = pixelBounds({forward.apply(left, top), forward.apply(right, top), forward.apply(right, bottom), forward.apply(left, bottom)},
                                        d.width, d.height);
    if (box.empty())
        return true;
    const float values[20] = {
        static_cast<float>(inverse.a), static_cast<float>(inverse.c), static_cast<float>(inverse.tx - text.x), 0.0f,
        static_cast<float>(inverse.b), static_cast<float>(inverse.d), static_cast<float>(inverse.ty - text.y), 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(op), 0.0f, static_cast<float>(text.width), static_cast<float>(text.height),
    };
    d.context->UpdateSubresource(d.layerParams.Get(), 0, nullptr, values, 0, 0);
    ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, nullptr, text.view.Get()};
    d.pass(d.rgbTarget.Get(), box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0, d.psText.Get(), views, 4, d.textBlend.Get());
    return true;
}

}  // namespace editor::core

#else  // altri sistemi

namespace editor::core {

bool GpuFrameConverter::clearCanvas(float, float, float) { return false; }
bool GpuFrameConverter::setCanvasBgra(const BgraImage&) { return false; }
bool GpuFrameConverter::drawLayer(const BgraImage&, const Transform&) { return false; }
bool GpuFrameConverter::drawNv12(const AVFrame*, const GpuColorSpace&, const Transform&) { return false; }
bool GpuFrameConverter::drawD3d11Frame(const AVFrame*, const GpuColorSpace&, const Transform&, std::string* why)
{
    if (why != nullptr)
        *why = "solo Windows";
    return false;
}
std::shared_ptr<GpuTextImage> GpuFrameConverter::createText(const TextImage&) { return nullptr; }
bool GpuFrameConverter::supportsTransition(const std::string&) { return false; }
bool GpuFrameConverter::blendTransition(GpuFrameConverter&, const std::string&, double, const TransitionParams&) { return false; }
bool GpuFrameConverter::drawText(const GpuTextImage&, const Transform&, double, double, double) { return false; }

}  // namespace editor::core

#endif
