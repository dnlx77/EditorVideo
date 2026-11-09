#pragma once

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/TextImage.h"
#include "EditorCore/Transform.h"
#include "EditorCore/VideoDecoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace editor::core {

// Una scritta già disegnata e quanto si vede (0..1: le dissolvenze).
struct BlendedText
{
    const TextImage* image = nullptr;  // già trasformata (blendTextOver); nullptr se si dà `base`
    double opacity = 1.0;
    int level = 0;  // il piano della sua corsia (0 = V1, 1 = V2...): copre i pezzi dei livelli più bassi e sta sotto quelli dei più alti
    // In alternativa a `image`: la scritta disegnata e NON trasformata, con la trasformazione da applicarle (vedi transformTextImage)
    // attorno a (pivotX, pivotY): ci pensa il compositore (la GPU lo fa a ogni fotogramma senza costo, la CPU con transformTextImage).
    const TextImage* base = nullptr;
    Transform transform;
    double pivotX = 0.0;
    double pivotY = 0.0;
    // Per la GPU: diverso da 0 = `base` (o `image`) è sempre la stessa immagine per questo numero finché la scritta resta visibile,
    // quindi si può tenere nella scheda video invece di ricaricarla a ogni fotogramma.
    std::uint64_t cacheId = 0;
};

// Un pezzo di video di un livello sopra V1 già decodificato (BGRA) e come va messo sul fotogramma.
struct BlendedLayer
{
    const VideoFrame* frame = nullptr;
    Transform transform;
    int level = 1;  // il piano del livello (1 = V2...)
};

// Dove va il tempo della composizione (secondi di orologio, sommati su tutti i fotogrammi): conversione dell'originale in RGBA,
// riempimento del nero, disegno del pezzo trasformato, livelli e scritte sopra, conversione in YUV per l'encoder.
struct CompositorTiming
{
    double toRgba = 0.0;
    double fill = 0.0;
    double composite = 0.0;
    double overlays = 0.0;
    double toYuv = 0.0;
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;  // formato dei fotogrammi dell'originale (per capire le conversioni lente)
};

// Chi compone i fotogrammi dell'esportazione: trasformazione del pezzo di V1, livelli, scritte e transizioni già fuse, e dà
// il fotogramma finito nel formato dell'encoder. Ha più realizzazioni (CPU: TextFrameBlender; GPU in arrivo) con lo stesso
// risultato; la CPU è il riferimento e il ripiego quando la scheda video non c'è o non si può usare.
// Non è thread-safe: un thread alla volta. Il fotogramma restituito vale fino alla chiamata successiva.
class IFrameCompositor
{
public:
    virtual ~IFrameCompositor() = default;

    // Nome per le statistiche e le impostazioni («CPU», poi il nome della scheda video).
    virtual const char* name() const = 0;
    virtual bool isHardware() const { return false; }
    // Se la GPU è stata abbandonata per la CPU a metà lavoro: il motivo; altrimenti vuoto.
    virtual std::string fallbackReason() const { return {}; }

    // I livelli da mettere sopra il fotogramma della prossima chiamata di blend*() (dal più basso al più alto; le scritte si
    // mettono in mezzo secondo il loro piano); valgono per quella chiamata sola.
    virtual void setLayers(std::vector<BlendedLayer> layers) = 0;
    // `source` con `texts` sopra (nell'ordine dato: l'ultima è la più alta); tempi e proprietà di `source`.
    virtual const AVFrame* blend(const AVFrame* source, const std::vector<BlendedText>& texts) = 0;
    // Come blend(), ma il fotogramma è già un'immagine BGRA della misura del progetto (es. una transizione già fusa);
    // `props` dà tempi e proprietà del risultato.
    virtual const AVFrame* blendRaster(const std::uint8_t* bgra, int stride, int width, int height,
                                       const std::vector<BlendedText>& texts, const AVFrame* props) = 0;
    // Come blend(), ma `source` prima si trasforma (vedi compositeLayer) e si disegna sul nero a piena risoluzione.
    virtual const AVFrame* blendTransformed(const AVFrame* source, const Transform& transform,
                                            const std::vector<BlendedText>& texts) = 0;

    virtual const CompositorTiming& timing() const = 0;
};

// Il compositore da usare per fotogrammi di `width` x `height` che escono in `pixelFormat`: quello su GPU (Direct3D 11) se
// `allowGpu`, c'è una scheda video utilizzabile e il formato è YUV 4:2:0 a 8 bit (yuv420p o nv12); altrimenti la CPU.
std::unique_ptr<IFrameCompositor> makeFrameCompositor(int width, int height, AVPixelFormat pixelFormat, bool allowGpu = true);

}  // namespace editor::core
