#pragma once

#include "EditorCore/FrameCompositor.h"
#include "EditorCore/GpuColor.h"
#include "EditorCore/GpuDevice.h"
#include "EditorCore/TextFrame.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace editor::core {

// Il compositore su GPU (Direct3D 11): V1 (adattato o trasformato), livelli, scritte e transizioni già fuse si compongono in
// RGBA16F sulla scheda video e il risultato torna in memoria nel formato dell'encoder (yuv420p o nv12, 8 bit). Stessi colori
// della CPU (BT.601, gamma limitata come la conversione di swscale che usa TextFrameBlender).
// Il ripiego sulla CPU è sempre a portata di mano:
// - un fotogramma in un formato che la GPU non gestisce (10 bit, 4:2:2...) si fa con TextFrameBlender, solo lui;
// - se la GPU dà un errore (dispositivo perso, memoria...) si passa alla CPU per tutto il resto dell'esportazione, e
//   fallbackReason() dice perché. Ogni chiamata di blend*() è completa in sé, quindi il passaggio non si vede nel risultato.
class GpuCompositor final : public IFrameCompositor
{
public:
    // nullptr se non c'è una GPU utilizzabile o il formato d'uscita non è yuv420p/nv12; `why` dice perché.
    // `allowSoftware`: solo per le prove (il rasterizzatore software della CI).
    static std::unique_ptr<GpuCompositor> create(int width, int height, AVPixelFormat pixelFormat, bool allowSoftware = false,
                                                 std::string* why = nullptr);
    ~GpuCompositor() override;

    const char* name() const override { return name_.c_str(); }
    bool isHardware() const override { return !failed_; }
    std::string fallbackReason() const override { return failure_; }

    void setLayers(std::vector<BlendedLayer> layers) override { layers_ = std::move(layers); }
    const AVFrame* blend(const AVFrame* source, const std::vector<BlendedText>& texts) override;
    const AVFrame* blendRaster(const std::uint8_t* bgra, int stride, int width, int height, const std::vector<BlendedText>& texts,
                               const AVFrame* props) override;
    const AVFrame* blendTransformed(const AVFrame* source, const Transform& transform, const std::vector<BlendedText>& texts) override;
    const CompositorTiming& timing() const override { return timing_; }

private:
    GpuCompositor(int width, int height, AVPixelFormat pixelFormat);

    // Compone un fotogramma sulla GPU: `source` (in NV12 o yuv420p, in memoria o della scheda video) con `transform`, oppure il
    // BGRA `raster` (se non nullo); poi livelli e scritte; poi il risultato nel formato d'uscita. nullptr se la GPU non può
    // (formato non gestito: `unsupported`; errore: `failed_`).
    const AVFrame* compose(const AVFrame* source, const std::uint8_t* raster, int rasterStride, const Transform& transform,
                           const std::vector<BlendedText>& texts, const AVFrame* props);
    const AVFrame* nv12Of(const AVFrame* source, bool& fullRange);
    bool drawOverlays(const std::vector<BlendedText>& texts);
    bool drawText(const BlendedText& text);
    TextFrameBlender& cpu();
    void fail(const std::string& reason);

    int width_;
    int height_;
    AVPixelFormat pixelFormat_;
    std::string name_;
    bool failed_ = false;
    std::string failure_;
    std::unique_ptr<GpuDevice> device_;
    std::unique_ptr<GpuFrameConverter> canvas_;
    std::unique_ptr<TextFrameBlender> cpu_;
    std::vector<BlendedLayer> layers_;
    std::map<std::uint64_t, std::shared_ptr<GpuTextImage>> textCache_;
    detail::FramePtr software_;   // il fotogramma della scheda video letto in memoria
    detail::FramePtr nv12In_;     // il fotogramma yuv420p riordinato in NV12
    detail::FramePtr nv12Out_;    // il risultato in NV12 (se l'uscita è yuv420p)
    detail::FramePtr output_;
    CompositorTiming timing_;
};

}  // namespace editor::core
