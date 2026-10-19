#pragma once

#include "EditorCore/MediaProbe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace editor::core {

// Caratteristiche di un flusso che devono coincidere per poter unire più file senza ricodifica.
struct StreamSignature
{
    StreamKind kind = StreamKind::Other;
    std::string codec;
    std::string profile;  // vuoto se sconosciuto
    std::vector<std::uint8_t> extradata;  // parametri di codifica (per H.264/HEVC: SPS/PPS)

    // Video
    int width = 0;
    int height = 0;
    std::string pixelFormat;
    double frameRate = 0.0;

    // Audio
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout;
};

struct MediaSignature
{
    std::string path;      // UTF-8
    std::string fileName;  // solo il nome, per i messaggi
    std::vector<StreamSignature> streams;  // solo video (non copertine) e audio, nell'ordine del file
};

// Una differenza che impedisce di unire un file al primo.
struct Mismatch
{
    int clipIndex = 0;     // 0 = primo file
    std::string message;   // es. "Clip 3 (nome.mp4): risoluzione 1280x720 invece di 1920x1080"
};

// Legge le caratteristiche dei flussi (veloce: non legge tutto il file). Lancia MediaError se il file non si apre.
MediaSignature readMediaSignature(const std::string& utf8Path);

// Confronta ogni file con il primo. Vuoto = si possono unire. Il confronto della base dei tempi non serve:
// nell'unione i tempi vengono convertiti comunque.
std::vector<Mismatch> checkCompatibility(const std::vector<MediaSignature>& clips);

// Tutte le differenze, una per riga.
std::string describeMismatches(const std::vector<Mismatch>& mismatches);

}  // namespace editor::core
