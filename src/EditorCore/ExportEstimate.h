#pragma once

#include "EditorCore/Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace editor::core {

// Che cosa serve per stimare la dimensione del file creato (solo una stima: dipende molto dal contenuto del video).
struct SizeEstimateInput
{
    VideoCodec codec = VideoCodec::H264;
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    int cq = 0;                    // qualità scelta (scala dell'encoder; meno = migliore)
    double copiedSeconds = 0.0;    // pezzi copiati così come sono
    double recodedSeconds = 0.0;   // tutto il resto (ricodificato, nero, scritte, transizioni)
    double copiedBitRate = 0.0;    // bit/s medi dei file originali (video e audio insieme), 0 = sconosciuto
    int recodedAudioBitRate = 0;   // bit/s dell'audio nei tratti ricodificati (0 = senza audio)
};

// Bit per pixel di ogni fotogramma a una qualità: parte da un valore tipico per il codec e dimezza ogni `step` punti di CQ.
inline double bitsPerPixel(VideoCodec codec, int cq)
{
    struct Model
    {
        int reference;
        double bpp;
        double step;
    };
    const Model model = codec == VideoCodec::Av1    ? Model{35, 0.050, 8.0}
                        : codec == VideoCodec::Hevc ? Model{28, 0.060, 6.0}
                                                    : Model{23, 0.100, 6.0};
    return model.bpp * std::pow(2.0, (model.reference - cq) / model.step);
}

// Dimensione stimata in byte. 0 se non si può stimare (nessuna misura o durata).
inline std::int64_t estimateOutputBytes(const SizeEstimateInput& in)
{
    double bits = in.copiedSeconds * std::max(0.0, in.copiedBitRate);
    if (in.recodedSeconds > 0.0 && in.width > 0 && in.height > 0 && in.frameRate > 0.0) {
        const double videoRate = bitsPerPixel(in.codec, in.cq) * in.width * in.height * in.frameRate;
        bits += in.recodedSeconds * (videoRate + std::max(0, in.recodedAudioBitRate));
    }
    return static_cast<std::int64_t>(std::llround(bits / 8.0));
}

}  // namespace editor::core
