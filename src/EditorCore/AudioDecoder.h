#pragma once

#include "EditorCore/MediaError.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace editor::core {

// Blocco di audio decodificato: PCM 16 bit con i canali interlacciati.
struct AudioChunk
{
    double time = 0.0;  // secondi dall'inizio del file del primo campione del blocco
    std::vector<std::int16_t> samples;
};

// Decodifica il flusso audio principale e lo converte in PCM 16 bit, stereo (o mono se la sorgente è mono),
// alla frequenza di campionamento originale. Non è thread-safe: va usato da un thread alla volta.
class AudioDecoder
{
public:
    // Lancia MediaError se il file non si apre o non ha un flusso audio.
    explicit AudioDecoder(const std::string& utf8Path);
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    int sampleRate() const;
    int channels() const;  // 1 o 2
    std::string codecName() const;
    double durationSeconds() const;  // -1 se sconosciuta

    // Si posiziona esattamente su `seconds`: il prossimo blocco parte da lì (i campioni precedenti vengono scartati).
    void seek(double seconds);

    // Prossimo blocco di audio; std::nullopt a fine file.
    std::optional<AudioChunk> next();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace editor::core
