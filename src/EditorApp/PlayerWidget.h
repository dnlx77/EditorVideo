#pragma once

#include "EditorApp/AudioOutput.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/TimeUtil.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

class QLabel;
class QTimer;

namespace editor::core {
class AudioDecoder;
class VideoDecoder;
struct VideoFrame;
}  // namespace editor::core

namespace editor::app {

class TimelineWidget;
class TransportBar;
class VideoView;

// Anteprima con riproduzione audio/video sincronizzata, linea del tempo con i fotogrammi chiave e segmento IN-OUT.
//
// Due thread di lavoro: uno decodifica il video in una piccola coda, l'altro decodifica l'audio e lo manda
// alla scheda audio. Un timer nel thread dell'interfaccia legge l'orologio (quanto audio è già stato suonato,
// oppure il tempo trascorso se il file non ha audio) e mostra il fotogramma giusto per quell'istante.
class PlayerWidget : public QWidget
{
public:
    explicit PlayerWidget(QWidget* parent = nullptr);
    ~PlayerWidget() override;

    void openFile(const QString& path);
    void closeFile();
    bool hasMedia() const;

    void togglePlayPause();  // mettendo in pausa la velocità torna quella scelta (finisce la spola)
    void play();
    void pause();
    // Ferma e torna dove era partita l'ultima riproduzione.
    void stop();

    // Velocità. setBaseSpeed: quella scelta dal menu (positiva), con cui parte la riproduzione. setSpeed: quella del
    // momento, anche negativa (all'indietro); se si sta riproducendo si continua da qui alla nuova velocità.
    // L'audio si sente solo fino a 2x in avanti (core::audibleAtSpeed), con la stessa intonazione.
    void setBaseSpeed(double speed);
    void setSpeed(double speed);
    double speed() const { return speed_; }
    double baseSpeed() const { return baseSpeed_; }
    // Spola (J = -1, L = +1, premuti più volte): vedi core::shuttleSpeed. Se si è fermi parte la riproduzione.
    void shuttle(int direction);
    // Ripetizione: arrivata a OUT (o alla fine del file) la riproduzione riparte da IN (o dall'inizio).
    void setLoop(bool loop);
    bool loop() const { return loop_; }
    // Volume dell'anteprima (0-100) e muto: solo l'ascolto, non il file esportato.
    void setVolume(int percent);
    void setMuted(bool muted);

    // Riproduce solo fra `start` ed `end` (secondi del file), poi si ferma e chiama onWindowEnd: serve all'anteprima
    // della sequenza, che passa al pezzo successivo (o al precedente, all'indietro). end <= 0 = nessun limite.
    void setPlaybackWindow(double start, double end);
    std::function<void()> onWindowEnd;
    // Pulsante "aggiungi al montaggio" della barra dei comandi.
    std::function<void()> onAddRequested;

    // Da dove leggere il video di un file (il suo proxy, se c'è); nessuna funzione = dal file stesso. L'audio viene
    // sempre dal file.
    std::function<QString(const QString& path)> videoSourceFor;
    // Il file da cui leggere il video può essere cambiato (proxy pronto, qualità cambiata): se è così si riapre il
    // video e si resta allo stesso punto, anche durante la riproduzione.
    void reloadVideo();
    // Menu "Qualità anteprima" della barra dei comandi: 1 = originale, 2, 4, 8 (frazione della risoluzione).
    void setPreviewQuality(int divisor);
    std::function<void(int divisor)> onPreviewQualityChosen;

    // Solo nero al posto dell'immagine (uno spazio vuoto del montaggio), senza chiudere il file.
    void setBlank(bool blank);

    // Nasconde la linea del tempo e i comandi (l'anteprima della sequenza ha i suoi).
    void setChromeVisible(bool visible);
    // Imposta IN e OUT (es. per riprendere un pezzo della sequenza) e una nota sotto la linea del tempo.
    void setInOut(double in, double out);
    void showNote(const QString& note);

    // Audio dell'anteprima del montaggio (da chiamare a riproduzione ferma):
    // tratti senza audio, nei tempi del file aperto (lì si sente silenzio)...
    void setAudioMutes(std::vector<core::TimeRange> ranges);
    // ...e un altro file al posto dell'audio del file: al tempo t del file si sente il tempo t + delta di `path`
    // (silenzio dove `path` non arriva). path vuoto = l'audio del file.
    void setExternalAudio(const QString& path, double delta);

    // Per l'anteprima del montaggio: l'audio lo suona il mixer della sequenza (SequenceAudio), non questo lettore.
    // setAudioEnabled(false): il lettore non apre né suona nessun audio. setExternalClock: l'orologio della
    // riproduzione è questo (secondi della sequenza, quindi dell'audio che si sente) meno `delta` (secondi della
    // sequenza meno secondi del file); nessuna funzione = l'orologio di prima (audio del file o tempo reale). Da
    // chiamare a riproduzione ferma. La funzione può restituire std::nullopt (es. l'audio non sta suonando): allora
    // l'orologio è quello di sempre (tempo reale).
    void setAudioEnabled(bool enabled);
    void setExternalClock(std::function<std::optional<double>()> sequenceClock, double delta);
    void setAudioNote(const QString& note);

    void seekTo(double seconds);
    void seekRelative(double deltaSeconds);

    bool isPlaying() const { return playing_; }
    // Tratto del video già in memoria (vedi core::VideoDecoder::cachedRange), nei tempi del file; {0, 0} se nessuno.
    std::pair<double, double> cachedRange() const;
    double currentPosition();  // secondi
    QString statusText() const;  // es. "Decodifica: GPU (D3D11VA)"

    // Segmento da tenere. IN resta dove lo si mette (taglio preciso), anche prima che arrivino i fotogrammi chiave;
    // OUT comprende il fotogramma visibile. Si lavora sul fotogramma che si vede.
    void setKeyframes(const std::vector<double>& keyframes);  // dall'analisi del file
    void markIn();
    void markOut();
    void clearSegment();
    void goToIn();
    void goToOut();
    void stepFrame(int direction);  // +1 fotogramma successivo, -1 precedente (mette in pausa)
    void stepToKeyframe(int direction);  // fotogramma chiave successivo (+1) o precedente (-1)
    void seekToEnd();                    // ultimo fotogramma
    double frameDuration() const { return frameDuration_; }
    TimelineWidget* timeline() const { return timeline_; }  // per lo zoom (menu e prove)
    // Doppio clic sull'immagine (schermo intero).
    std::function<void()> onVideoDoubleClicked;
    // Cosa scrivere nell'anteprima quando non c'è nessun file.
    void setIdleMessage(const QString& message);

    // Può diventare molto piccola: quando lo spazio è poco nasconde prima le righe secondarie (segmento, decoder) e poi
    // la linea del tempo, lasciando l'immagine e i comandi di riproduzione.
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

public:
    const core::CutSegment& segment() const;

    // Tratti del file che sono nel montaggio (bande verdi), in ordine e già uniti. Va chiamato dopo setKeyframes(),
    // che li toglie.
    void setRanges(std::vector<core::TimeRange> ranges);
    std::function<void()> onSegmentChanged;  // IN, OUT o elenco dei tratti cambiati (anche aprendo un altro file)

private:
    // L'audio da riprodurre: quello del file o quello esterno. `own` = decoder dell'audio del file già aperto (si usa
    // se non c'è un audio esterno, invece di riaprire il file).
    void openAudio(std::unique_ptr<core::AudioDecoder> own = nullptr);
    void updateCompact();  // cosa mostrare per la misura attuale
    void startWorkers(double from);
    void stopWorkers();
    void videoWorker();
    void videoReverseWorker(double from);  // all'indietro: blocchi decodificati in avanti e poi mostrati al contrario
    void audioWorker();
    void restartAt(double position);  // durante la riproduzione: riparte da `position` (salto, ripetizione, velocità)
    void updateGain();

    double clock();
    void tick();
    QElapsedTimer labelClock_;  // ultimo aggiornamento dell'etichetta del tempo durante la riproduzione
    // Ferma la riproduzione in `position`; con signalWindowEnd avvisa poi onWindowEnd (fine del pezzo).
    void stopAt(double position, bool signalWindowEnd);
    void showFrameAt(double seconds);
    void showDecodeError(const std::exception& e);
    void updateControls();
    void updatePositionLabel(double seconds);
    void updateSegmentUi();
    double markTime() const;

    VideoView* view_ = nullptr;
    TransportBar* transport_ = nullptr;
    TimelineWidget* timeline_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* segmentLabel_ = nullptr;
    QWidget* controls_ = nullptr;
    QLabel* decoderLabel_ = nullptr;
    QTimer* timer_ = nullptr;
    QTimer* resizeRefresh_ = nullptr;

    std::unique_ptr<core::VideoDecoder> video_;
    std::unique_ptr<core::AudioDecoder> audio_;
    AudioOutput audioOut_;
    QString path_;                   // file aperto
    QString videoPath_;              // file da cui si legge il video: path_ o il suo proxy
    double ownAudioDuration_ = 0.0;  // durata dell'audio del file (anche quando suona quello esterno)
    bool ownAudio_ = false;          // il file ha un audio
    bool audioEnabled_ = true;                 // vedi setAudioEnabled()
    std::function<std::optional<double>()> externalClock_;  // vedi setExternalClock()
    double externalClockDelta_ = 0.0;
    std::vector<core::TimeRange> audioMutes_;
    QString externalAudio_;
    double externalDelta_ = 0.0;
    bool externalActive_ = false;    // audio_ è quello esterno
    QString idleMessage_;
    bool chromeVisible_ = true;  // vedi setChromeVisible
    QString audioNote_;    // es. "audio non disponibile"
    QString decoderText_;  // "Decodifica: GPU (D3D11VA)" / "Decodifica: software"

    double duration_ = 0.0;
    double position_ = 0.0;
    double displayedTime_ = 0.0;      // tempo del fotogramma visibile
    double frameDuration_ = 0.04;
    bool decoderAtDisplayed_ = false; // il decoder è subito dopo il fotogramma visibile
    bool playing_ = false;
    double windowStart_ = 0.0;  // vedi setPlaybackWindow()
    double windowEnd_ = 0.0;
    double speed_ = 1.0;        // vedi setSpeed()
    double baseSpeed_ = 1.0;
    bool loop_ = false;
    int volume_ = 100;
    bool muted_ = false;
    double playStart_ = 0.0;    // da dove è partita l'ultima riproduzione (per stop)
    bool hasPlayStart_ = false;

    core::CutSegment segment_;
    std::vector<core::TimeRange> ranges_;  // vedi setRanges()
    QString segmentNote_;  // spiegazione dell'ultima operazione su IN/OUT (es. IN agganciato)
    bool noteIsOutError_ = false;

    // Stato condiviso con i thread di lavoro.
    std::thread videoThread_;
    std::thread audioThread_;
    std::atomic<bool> stopWorkers_{false};
    std::mutex queueMutex_;
    std::condition_variable queueSpace_;
    std::deque<std::shared_ptr<core::VideoFrame>> queue_;
    bool videoEnded_ = true;               // protetto da queueMutex_
    std::atomic<bool> audioEnded_{true};
    std::atomic<double> audioStart_{0.0};  // tempo del primo campione mandato alla scheda audio
    double audioFrom_ = 0.0;   // da dove parte la riproduzione (per il silenzio prima dell'audio esterno)
    double audioDelta_ = 0.0;  // tempo nell'audio = tempo del file + audioDelta_
    std::vector<core::TimeRange> workerMutes_;  // copia di audioMutes_ per il thread audio
    double workerSpeed_ = 1.0;  // velocità della riproduzione in corso (ferma mentre i thread lavorano)
    std::atomic<float> gain_{1.0f};  // volume per il thread audio

    // Orologio: audio se c'è, altrimenti tempo reale.
    bool audioActive_ = false;
    bool audioClock_ = false;
    double wallBase_ = 0.0;
    std::chrono::steady_clock::time_point wallStart_;
};

}  // namespace editor::app
