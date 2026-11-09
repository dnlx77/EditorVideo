#pragma once

// Solo Windows: i tipi D3D11 dietro GpuDevice e GpuFrameConverter.
#ifdef _WIN32

#include "EditorCore/Compositor.h"
#include "EditorCore/GpuColor.h"
#include "EditorCore/GpuDevice.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <map>
#include <utility>

namespace editor::core {

struct GpuDevice::Impl
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

// Vedi GpuFrameConverter::createText.
class GpuTextImage
{
public:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    int x = 0;  // dove sta il riquadro nel fotogramma (come TextImage)
    int y = 0;
    int width = 0;
    int height = 0;
};

struct GpuFrameConverter::Impl
{
    GpuDevice* gpu = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    int width = 0;
    int height = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psToRgb, psToY, psToUV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> yTex, uvTex, rgbTex, yOut, uvOut;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yView, uvView, rgbView;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rgbTarget, yTarget, uvTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> yStage, uvStage, rgbStage;

    // I livelli (GpuLayer.cpp): creati al primo uso.
    struct SourceTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };
    bool layerReady = false;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psLayer, psCopy, psText;
    Microsoft::WRL::ComPtr<ID3D11BlendState> textBlend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> borderSampler;  // fuori dalla scritta è trasparente
    Microsoft::WRL::ComPtr<ID3D11Buffer> layerParams;
    Microsoft::WRL::ComPtr<ID3D11BlendState> layerBlend;
    std::map<std::pair<int, int>, SourceTexture> sources;  // per misura, riusate fra un livello e l'altro

    Microsoft::WRL::ComPtr<ID3D11Texture2D> makeTexture(int w, int h, DXGI_FORMAT format, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                                                        UINT cpu = 0, UINT mips = 1, UINT misc = 0);
    bool init();
    void setParams(const GpuColorSpace& colors);
    // Un passaggio a tutto schermo su `target` nel rettangolo (vx, vy, vw, vh) con `shader`; `views` sono i t0..t(viewCount-1).
    void pass(ID3D11RenderTargetView* target, int vx, int vy, int vw, int vh, ID3D11PixelShader* shader,
              ID3D11ShaderResourceView* const* views, UINT viewCount, ID3D11BlendState* blend = nullptr);
    bool readStage(ID3D11Texture2D* stage, std::uint8_t* dst, int dstStride, int rows, int bytesPerRow);
    bool ensureLayerResources();
    // Una texture BGRA 8 bit di quella misura (con tutti i livelli di mip) con `source` caricato dentro.
    SourceTexture* uploadSource(const BgraImage& source, bool mips);
};

// Compila una funzione degli shader (nullptr se non compila).
Microsoft::WRL::ComPtr<ID3DBlob> compileShader(const char* source, const char* entry, const char* target);

}  // namespace editor::core

#endif
