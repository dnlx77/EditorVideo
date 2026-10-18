#pragma once

#include "EditorCore/MediaError.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace editor::core {

// Fotogramma decodificato, convertito in BGRA 8 bit (lo stesso layout di QImage::Format_RGB32).
struct VideoFrame
{
    double time = 0.0;  // secondi dall'inizio del file (stessa convenzione dei fotogrammi chiave)
    int width = 0;
    int height = 0;
    int stride = 0;     // byte per riga
    std::vector<std::uint8_t> pixels;
};

// Decodifica il flusso video principale di un file.
// Usa la GPU (D3D11VA) quando il codec e la scheda video lo permettono, altrimenti la CPU:
// il passaggio al software è automatico, anche se la GPU fallisce durante la decodifica.
// Non è thread-safe: va usato da un thread alla volta.
class VideoDecoder
{
public:
    struct Options
    {
        bool allowHardware = true;
        // I fotogrammi vengono rimpiccioliti (mantenendo le proporzioni) per stare in questo riquadro.
        // Si può cambiare in seguito con setOutputSize().
        int maxWidth = 1920;
        int maxHeight = 1080;
    };

    // Lancia MediaError se il file non si apre o non ha un flusso video.
    explicit VideoDecoder(const std::string& utf8Path);
    VideoDecoder(const std::string& utf8Path, const Options& options);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // true se in questo momento sta decodificando con la GPU.
    bool usingHardware() const;
    // "GPU (D3D11VA)" oppure "software".
    std::string decoderDescription() const;

    std::string codecName() const;
    double durationSeconds() const;  // -1 se sconosciuta
    double frameRate() const;        // 0 se sconosciuta
    int outputWidth() const;         // dimensioni dei fotogrammi restituiti (proporzioni di visualizzazione già applicate)
    int outputHeight() const;

    // Riquadro in cui far stare i fotogrammi (mantenendo le proporzioni, anche ingrandendo): di solito la
    // dimensione in pixel dell'area di anteprima, così l'interfaccia li disegna senza doverli riscalare.
    // È l'unico metodo che si può chiamare da un altro thread mentre si decodifica.
    void setOutputSize(int boxWidth, int boxHeight);

    // Si posiziona su `seconds` e restituisce il fotogramma più vicino a quel tempo.
    // Oltre la fine restituisce l'ultimo fotogramma. nullptr solo se il video non ha fotogrammi decodificabili.
    std::shared_ptr<VideoFrame> seek(double seconds);

    // Fotogramma successivo a quello restituito per ultimo; nullptr a fine file.
    std::shared_ptr<VideoFrame> nextFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// true se FFmpeg è stato compilato con il supporto D3D11VA.
bool hardwareDecodingCompiledIn();

}  // namespace editor::core
