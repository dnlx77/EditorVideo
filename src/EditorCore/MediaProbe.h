#pragma once

#include "EditorCore/MediaError.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace editor::core {

enum class StreamKind
{
    Video,
    Audio,
    Subtitle,
    Other,
};

// Informazioni su un singolo flusso del file. I campi che non hanno senso per il tipo di flusso restano a 0 / vuoti.
struct StreamInfo
{
    int index = 0;
    StreamKind kind = StreamKind::Other;
    std::string codec;          // es. "h264"
    std::string codecLongName;  // es. "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"
    std::string profile;        // es. "High", vuoto se sconosciuto
    std::string language;       // tag "language", es. "ita"
    std::int64_t bitRate = 0;   // bit/s, 0 se sconosciuto
    double durationSeconds = -1.0;  // -1 se sconosciuta

    // Video
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    std::string pixelFormat;    // es. "yuv420p"
    bool attachedPicture = false;  // copertina (immagine allegata), non un vero video

    // Audio
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout;  // es. "stereo", "5.1(side)"
};

struct MediaInfo
{
    std::string path;            // UTF-8
    std::string formatName;      // es. "mov,mp4,m4a,3gp,3g2,mj2"
    std::string formatLongName;  // es. "QuickTime / MOV"
    double durationSeconds = -1.0;
    std::int64_t bitRate = 0;    // bit/s complessivo, 0 se sconosciuto
    std::vector<StreamInfo> streams;

    // Fotogrammi chiave del flusso video principale (-1 se il file non ha video).
    // I tempi sono in secondi dall'inizio del file, come li mostra un lettore.
    int keyframeStreamIndex = -1;
    std::vector<double> keyframeTimes;
};

// Errore di analisi con un messaggio in italiano da mostrare all'utente.
class ProbeError : public MediaError
{
public:
    using MediaError::MediaError;
};

// Lanciata quando l'analisi viene interrotta tramite il flag di annullamento.
class ProbeCancelled : public ProbeError
{
public:
    ProbeCancelled();
};

// Analizza il file (percorso in UTF-8): contenitore, flussi e fotogrammi chiave.
// I fotogrammi chiave si ricavano leggendo i pacchetti del flusso video, senza decodificarli.
// Se `cancel` diventa true durante la lettura, lancia ProbeCancelled. Lancia ProbeError per gli altri errori.
// Con `keyframeCacheFolder` (vedi KeyframeCache.h) i fotogrammi chiave già letti in passato si prendono da lì e quelli
// letti ora vengono salvati; vuota = nessuna cache.
MediaInfo probeFile(const std::string& utf8Path, const std::atomic<bool>* cancel = nullptr,
                    const std::string& keyframeCacheFolder = {});

const char* streamKindName(StreamKind kind);  // "Video", "Audio", "Sottotitoli", "Altro"

// "hh:mm:ss.mmm"; "sconosciuta" se negativa.
std::string formatDuration(double seconds);

// "850 kb/s", "12.5 Mb/s"; "sconosciuto" se 0.
std::string formatBitRate(std::int64_t bitsPerSecond);

// "25 fps", "29,97 fps", "23,976 fps"; "sconosciuto" se 0.
std::string formatFrameRate(double fps);

}  // namespace editor::core
