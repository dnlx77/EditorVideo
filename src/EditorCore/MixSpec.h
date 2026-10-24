#pragma once

#include "EditorCore/AudioTracks.h"
#include "EditorCore/TimeUtil.h"

#include <map>
#include <string>
#include <vector>

namespace editor::core {

// Frequenza e canali del mix (stereo a 48 kHz, come l'audio che si esporterà).
inline constexpr int kMixSampleRate = 48000;
inline constexpr int kMixChannels = 2;

// Un tratto di un file messo in un punto della sequenza: o l'audio di un pezzo video (traccia A1) o una clip di una
// traccia audio aggiunta.
struct MixSegment
{
    std::string path;        // UTF-8
    double fileStart = 0.0;  // nel file
    double fileEnd = 0.0;
    double position = 0.0;   // dove inizia nella sequenza
    int trackId = AudioTracks::kOriginalTrack;
    std::vector<TimeRange> muted;  // tratti senza audio, nei tempi del file

    double length() const { return fileEnd - fileStart; }
};

// La linea del livello di ogni traccia (vedi LevelPoint), per le tracce che ne hanno una.
using TrackEnvelopes = std::map<int, std::vector<LevelPoint>>;

// Tutto quello che serve per mixare l'audio della sequenza (anche in un altro thread, senza il progetto).
struct MixSpec
{
    std::vector<MixSegment> segments;
    std::map<int, float> gains;  // moltiplicatore di ogni traccia (A1 compresa), 0 = non si sente
    TrackEnvelopes envelopes;
    double length = 0.0;  // durata della sequenza, in secondi (l'audio esportato dura almeno quanto lei)
};

}  // namespace editor::core
