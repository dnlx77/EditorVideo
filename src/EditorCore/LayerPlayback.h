#pragma once

#include "EditorCore/VideoDecoder.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace editor::core {

// I fotogrammi di un file per un pezzo di video su un livello sopra V1, preparati in un thread a parte (ogni livello è una
// decodifica in più: nel thread dell'interfaccia l'immagine e l'audio si fermerebbero). Il file si apre nel thread, alla
// prima richiesta. Tiene pochi fotogrammi in coda: chi riproduce chiede quello del momento con frame().
class LayerPlayback
{
public:
    // `box`: riquadro in cui far stare i fotogrammi (vedi VideoDecoder::setOutputSize); `utf8Path` è il file da leggere (di
    // solito il proxy).
    LayerPlayback(std::string utf8Path, int boxWidth, int boxHeight, bool allowHardware = true);
    ~LayerPlayback();
    LayerPlayback(const LayerPlayback&) = delete;
    LayerPlayback& operator=(const LayerPlayback&) = delete;

    const std::string& path() const { return path_; }
    void setBox(int width, int height);

    // Il tempo del file che serve adesso (senza aspettare). In pausa (`playing` falso) si va dritti al fotogramma di quel
    // tempo; in riproduzione si continua a decodificare in avanti e ci si sposta solo se si è troppo lontani (salto, o
    // indietro, o decodifica troppo in ritardo).
    void request(double fileTime, bool playing);
    // Il fotogramma da mostrare al tempo `fileTime` del file: l'ultimo in coda non dopo quel tempo (quelli già superati si
    // scartano, l'ultimo resta). nullptr se ancora non c'è.
    std::shared_ptr<VideoFrame> frame(double fileTime);
    // request() senza riproduzione e attesa del fotogramma (nullptr se non arriva entro `timeoutMs` o il file non si apre).
    std::shared_ptr<VideoFrame> waitFrame(double fileTime, int timeoutMs = 5000);
    // Il file non si apre o non ha video.
    bool failed() const { return failed_.load(); }

private:
    void run();

    static constexpr std::size_t kMaxQueued = 6;
    const std::string path_;
    const bool hardware_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;  // c'è da fare, o bisogna uscire
    std::condition_variable done_;  // è arrivato un fotogramma
    bool stop_ = false;
    bool playing_ = false;
    bool hasSeek_ = false;
    double seekTarget_ = 0.0;
    unsigned long long seekSerial_ = 0;
    bool ended_ = false;          // il file è finito (nessun altro fotogramma in avanti)
    bool decoding_ = false;       // c'è una decodifica in corso (fuori dal mutex)
    double lastRequested_ = -1.0;  // tempo dell'ultima richiesta: se scende, il file si legge all'indietro
    std::atomic<bool> backward_{false};
    double lastDecoded_ = -1.0;   // tempo dell'ultimo fotogramma decodificato (-1 = nessuno)
    double frameDuration_ = 0.04;
    int boxWidth_;
    int boxHeight_;
    bool boxChanged_ = false;
    std::deque<std::shared_ptr<VideoFrame>> queue_;
    std::atomic<bool> failed_{false};
};

}  // namespace editor::core
