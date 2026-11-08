#pragma once

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/TextImage.h"
#include "EditorCore/Transform.h"
#include "EditorCore/VideoDecoder.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace editor::core {

// Mette le scritte su un fotogramma da ricodificare (esportazione). Il fotogramma dell'originale diventa un fotogramma
// YUV della misura del progetto, con le stesse bande nere che metterebbe l'encoder se le proporzioni sono diverse,
// e le scritte fuse sopra (passando per RGBA). Serve solo per i fotogrammi con una scritta: gli altri vanno all'encoder
// come sono.
// Una scritta già disegnata e quanto si vede (0..1: le dissolvenze).
struct BlendedText
{
    const TextImage* image = nullptr;
    double opacity = 1.0;
    int level = 0;  // il piano della sua corsia (0 = V1, 1 = V2...): copre i pezzi dei livelli più bassi e sta sotto quelli dei più alti
};

// Un pezzo di video di un livello sopra V1 già decodificato (BGRA) e come va messo sul fotogramma.
struct BlendedLayer
{
    const VideoFrame* frame = nullptr;
    Transform transform;
    int level = 1;  // il piano del livello (1 = V2...)
};

class TextFrameBlender
{
public:
    // `pixelFormat`: il formato dei fotogrammi che escono (di solito quello del progetto); le scritte si fondono in RGBA
    // a 8 bit e poi si convertono, quindi in un formato a più bit cambia solo il formato di uscita.
    TextFrameBlender(int width, int height, AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P);

    // I pezzi dei livelli da mettere sopra il fotogramma della prossima chiamata di blend*() (dal più basso al più alto; le scritte
    // si mettono in mezzo secondo il loro piano); valgono per quella chiamata sola. Il fotogramma da cui si parte è il nero con i bordi o la trasformazione
    // di V1, o la transizione, come senza livelli.
    void setLayers(std::vector<BlendedLayer> layers) { layers_ = std::move(layers); }

    // `source` con `texts` sopra (nell'ordine dato: l'ultima è la più alta). Il risultato resta valido fino alla
    // prossima chiamata; ha i tempi (pts) e le proprietà di `source`.
    const AVFrame* blend(const AVFrame* source, const std::vector<BlendedText>& texts);

    // Come blend(), ma il fotogramma è già un'immagine BGRA della misura del progetto (width x height, `stride` byte per
    // riga), per esempio una transizione già fusa; `props` dà tempi e proprietà del risultato.
    const AVFrame* blendRaster(const std::uint8_t* bgra, int stride, int width, int height, const std::vector<BlendedText>& texts,
                               const AVFrame* props);

    // Come blend(), ma `source` prima si trasforma (scala, posizione, rotazione, ritaglio: vedi compositeLayer) e si disegna
    // sul nero a piena risoluzione; poi le scritte sopra. Una trasformazione nulla dà come blend() (le bande nere sono
    // quelle dell'adattamento al fotogramma del progetto).
    const AVFrame* blendTransformed(const AVFrame* source, const Transform& transform, const std::vector<BlendedText>& texts);

    // Dove va il tempo di blend*() (secondi di orologio, sommati su tutti i fotogrammi): conversione dell'originale in RGBA,
    // riempimento del nero, disegno del pezzo trasformato, livelli e scritte sopra, conversione in YUV per l'encoder.
    struct Timing
    {
        double toRgba = 0.0;
        double fill = 0.0;
        double composite = 0.0;
        double overlays = 0.0;
        double toYuv = 0.0;
    };
    const Timing& timing() const { return timing_; }

private:
    const AVFrame* toYuv(const std::vector<BlendedText>& texts, const AVFrame* props);

    int width_;
    int height_;
    AVPixelFormat pixelFormat_;
    std::vector<std::uint8_t> rgba_;
    std::vector<BlendedLayer> layers_;  // vedi setLayers
    std::vector<std::uint8_t> layer_;  // il fotogramma originale in RGBA, per blendTransformed
    detail::FramePtr software_;  // per i fotogrammi della scheda video
    detail::FramePtr output_;
    std::unique_ptr<SwsContext, detail::SwsDeleter> toRgba_;
    std::unique_ptr<SwsContext, detail::SwsDeleter> toYuv_;
    Timing timing_;
};

}  // namespace editor::core
