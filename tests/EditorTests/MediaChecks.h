#pragma once

// Controlli comuni ai test sui file creati (esportazioni e unioni).

#include "EditorCore/VideoDecoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace editor::tests {

// Luminanza (Y, range TV) del pixel (x, y), ricavata dal grigio BGRA del fotogramma.
double lumaAt(const core::VideoFrame& frame, int x, int y);

// Il video del file deve essere esattamente questa sequenza di fotogrammi dei file di prova (per numero), a tempi
// continui di 1/25 s (con tolleranza timeTolerance: alle giunzioni può esserci qualche millesimo in più).
void expectVideoFrames(const std::string& file, const std::vector<int>& frames, double timeTolerance = 0.002);

// Su ogni flusso i tempi di decodifica crescono sempre; il primo pacchetto video è un fotogramma chiave.
void expectMonotonicPackets(const std::string& file);

// Canale sinistro di tutto l'audio (PCM 16 bit) del file.
std::vector<std::int16_t> leftChannel(const std::string& file);

// Tempi di inizio dei blocchi audio decodificati, e se due blocchi si sovrappongono.
struct AudioTimeline
{
    std::vector<double> starts;
    double totalSeconds = 0.0;
    bool overlaps = false;
};
AudioTimeline audioTimeline(const std::string& file);

}  // namespace editor::tests
