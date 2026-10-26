#pragma once

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/TextImage.h"

#include <cstdint>
#include <vector>

namespace editor::core {

// Mette le scritte su un fotogramma da ricodificare (esportazione). Il fotogramma dell'originale diventa un fotogramma
// YUV 4:2:0 della misura del progetto, con le stesse bande nere che metterebbe l'encoder se le proporzioni sono diverse,
// e le scritte fuse sopra (passando per RGBA). Serve solo per i fotogrammi con una scritta: gli altri vanno all'encoder
// come sono.
class TextFrameBlender
{
public:
    TextFrameBlender(int width, int height);

    // `source` con `texts` sopra (nell'ordine dato: l'ultima è la più alta). Il risultato resta valido fino alla
    // prossima chiamata; ha i tempi (pts) e le proprietà di `source`.
    const AVFrame* blend(const AVFrame* source, const std::vector<const TextImage*>& texts);

private:
    int width_;
    int height_;
    std::vector<std::uint8_t> rgba_;
    detail::FramePtr software_;  // per i fotogrammi della scheda video
    detail::FramePtr output_;
    std::unique_ptr<SwsContext, detail::SwsDeleter> toRgba_;
    std::unique_ptr<SwsContext, detail::SwsDeleter> toYuv_;
};

}  // namespace editor::core
