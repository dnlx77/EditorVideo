#pragma once

#include "EditorCore/TransitionRender.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace editor::core {

// Fa i fotogrammi delle transizioni in un thread a parte (due decodifiche e la fusione per fotogramma sono troppo per il
// thread dell'interfaccia), così l'immagine e l'audio non si fermano. Il thread tiene una sola richiesta, l'ultima: se
// non fa in tempo salta i fotogrammi invece di accumularli.
class TransitionPlayback
{
public:
    struct Result
    {
        std::shared_ptr<VideoFrame> frame;  // nullptr se non si è riusciti (o ancora niente)
        double time = 0.0;                  // tempo della sequenza a cui si riferisce
        unsigned long long serial = 0;      // numero della richiesta
    };

    TransitionPlayback();
    ~TransitionPlayback();
    TransitionPlayback(const TransitionPlayback&) = delete;
    TransitionPlayback& operator=(const TransitionPlayback&) = delete;

    // Misura dei fotogrammi, origine del video (proxy) e GPU: vedi TransitionRenderer. Valgono dalla prossima richiesta.
    void setBox(int width, int height);
    void setVideoSource(std::function<std::string(const std::string& path)> source);
    void setHardwareAllowed(bool allowed);
    // Chiude i decoder (es. cambia il file da cui leggere).
    void reset();

    // Chiede il fotogramma al tempo dato (senza aspettare); sostituisce la richiesta precedente se non è ancora partita.
    // Restituisce il numero della richiesta.
    unsigned long long request(const TransitionJob& job, double sequenceTime);
    // Chiede e aspetta il fotogramma (nullptr se non arriva entro `timeoutMs` o non si riesce).
    std::shared_ptr<VideoFrame> render(const TransitionJob& job, double sequenceTime, int timeoutMs = 10000);
    // Flusso di fotogrammi per la riproduzione: da `from` (tempo della sequenza), avanti (direction > 0) o indietro, uno
    // ogni `step` secondi della sequenza, fino all'estremo della transizione. Il thread li prepara in anticipo (pochi,
    // in coda) con il loro tempo; chi riproduce prende quello del momento con takeFrame(). Una richiesta (request,
    // render) ferma il flusso.
    void startStream(const TransitionJob& job, double from, double step, int direction);
    // Il fotogramma da mostrare al tempo `time` della sequenza: l'ultimo in coda non dopo `time` (all'indietro: non
    // prima); quelli già superati si scartano e il thread ne prepara altri. Lo stesso fotogramma torna finché non ne
    // arriva uno nuovo. nullptr se non è ancora il momento del primo in coda (o non ce ne sono).
    std::shared_ptr<VideoFrame> takeFrame(double time);
    // L'ultimo risultato finito.
    Result latest() const;
    // Quanto ci mette in media a fare un fotogramma (secondi), per sapere se sta al passo.
    double averageRenderSeconds() const;

private:
    struct Request
    {
        TransitionJob job;
        double time = 0.0;
        unsigned long long serial = 0;
    };
    void run();
    bool streamWorkAvailable() const;  // il flusso ha ancora un fotogramma da preparare e posto in coda

    mutable std::mutex mutex_;
    std::condition_variable wake_;  // c'è una richiesta o bisogna uscire
    std::condition_variable done_;  // un risultato è pronto
    std::thread thread_;
    bool stop_ = false;
    bool hasRequest_ = false;
    Request request_;
    unsigned long long serial_ = 0;
    Result result_;
    double averageSeconds_ = 0.0;

    // Flusso (vedi startStream).
    static constexpr std::size_t kMaxQueued = 6;
    bool streamActive_ = false;
    TransitionJob streamJob_;
    double streamNext_ = 0.0;  // il prossimo tempo da preparare
    double streamStep_ = 0.04;
    int streamDirection_ = 1;
    double clockHint_ = 0.0;   // l'ultimo tempo chiesto con takeFrame: serve a saltare i fotogrammi in ritardo
    unsigned long long streamGeneration_ = 0;
    std::deque<std::shared_ptr<VideoFrame>> queue_;

    // Impostazioni da applicare al renderer prima della richiesta dopo (lo usa solo il thread).
    bool configChanged_ = false;
    int boxWidth_ = 1280;
    int boxHeight_ = 720;
    bool hardware_ = true;
    bool resetRequested_ = false;
    std::function<std::string(const std::string&)> source_;
    bool sourceChanged_ = false;
};

}  // namespace editor::core
