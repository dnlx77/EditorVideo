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
    bool bFrames = false;  // fotogrammi che guardano avanti (l'ordine di decodifica non è quello di visualizzazione)

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

// Legge le caratteristiche dei flussi (veloce: non legge tutto il file). Lancia MediaError se il file non si apre.
MediaSignature readMediaSignature(const std::string& utf8Path);

// Formato del progetto: codec, misure, fotogrammi al secondo e audio del file che si esporta. Lo dà un file del
// progetto (di solito quello del primo pezzo messo nel montaggio); i pezzi degli altri file si confrontano con questo.
struct ProjectFormat
{
    std::string sourceName;  // nome del file da cui è stato preso (per mostrarlo)
    std::vector<StreamSignature> streams;  // come in MediaSignature, con i parametri di codifica

    bool empty() const { return streams.empty(); }
    const StreamSignature* video() const;  // nullptr se non c'è
    const StreamSignature* audio() const;
};

ProjectFormat formatOf(const MediaSignature& signature);

// Come può entrare nel file esportato un pezzo di un file, rispetto al formato del progetto.
enum class ClipFit
{
    Copy,                // si copia così com'è
    CopyWithParameters,  // si copia, con i suoi parametri di codifica ripetuti nel flusso (solo parametri di codifica diversi)
    Reencode,            // diverso: senza ricodifica non si può unire
};

struct ClipFitResult
{
    ClipFit fit = ClipFit::Copy;
    std::vector<std::string> reasons;  // le differenze, in italiano (vuoto per Copy)
};

// withAudio = false: l'audio non conta (es. con una traccia audio sostitutiva l'audio dei file non si copia).
ClipFitResult classifyClip(const MediaSignature& clip, const ProjectFormat& format, bool withAudio = true);

}  // namespace editor::core
