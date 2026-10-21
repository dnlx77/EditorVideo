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
#include <thread>
#include <vector>

class QLabel;
class QTimer;
class QToolButton;

namespace editor::core {
class AudioDecoder;
class VideoDecoder;
struct VideoFrame;
}  // namespace editor::core

namespace editor::app {

class TimelineWidget;
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

    void togglePlayPause();
    void play();
    void pause();

    // Riproduce solo fino a `end` (secondi del file), poi si ferma e chiama onWindowEnd: serve all'anteprima della
    // sequenza, che passa al pezzo successivo. end <= 0 = nessun limite.
    void setPlaybackWindow(double end);
    std::function<void()> onWindowEnd;

    // Nasconde la linea del tempo e i comandi (l'anteprima della sequenza ha i suoi).
    void setChromeVisible(bool visible);
    // Imposta IN e OUT (es. per riprendere un pezzo della sequenza) e una nota sotto la linea del tempo.
    void setInOut(double in, double out);
    void showNote(const QString& note);
    // IN come l'ha segnato l'utente, prima dell'aggancio al fotogramma chiave (serve per togliere un pezzo).
    double wantedIn() const { return wantedIn_; }

    // Audio dell'anteprima del montaggio (da chiamare a riproduzione ferma):
    // tratti senza audio, nei tempi del file aperto (lì si sente silenzio)...
    void setAudioMutes(std::vector<core::TimeRange> ranges);
    // ...e un altro file al posto dell'audio del file: al tempo t del file si sente il tempo t + delta di `path`
    // (silenzio dove `path` non arriva). path vuoto = l'audio del file.
    void setExternalAudio(const QString& path, double delta);

    void seekTo(double seconds);
    void seekRelative(double deltaSeconds);

    bool isPlaying() const { return playing_; }
    double currentPosition();  // secondi
    QString statusText() const;  // es. "Decodifica: GPU (D3D11VA)"

    // Segmento da tenere. IN si aggancia al fotogramma chiave nello stesso punto o prima (taglio senza
    // ricodifica); OUT comprende il fotogramma visibile. Si lavora sul fotogramma che si vede.
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
    const core::CutSegment& segment() const;

    // Tratti del file che sono nel montaggio (bande verdi), in ordine e già uniti. Va chiamato dopo setKeyframes(),
    // che li toglie.
    void setRanges(std::vector<core::TimeRange> ranges);
    std::function<void()> onSegmentChanged;  // IN, OUT o elenco dei tratti cambiati (anche aprendo un altro file)

private:
    // L'audio da riprodurre: quello del file o quello esterno. `own` = decoder dell'audio del file già aperto (si usa
    // se non c'è un audio esterno, invece di riaprire il file).
    void openAudio(std::unique_ptr<core::AudioDecoder> own = nullptr);
    void startWorkers(double from);
    void stopWorkers();
    void videoWorker();
    void audioWorker();

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
    QToolButton* playButton_ = nullptr;
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
    double ownAudioDuration_ = 0.0;  // durata dell'audio del file (anche quando suona quello esterno)
    bool ownAudio_ = false;          // il file ha un audio
    std::vector<core::TimeRange> audioMutes_;
    QString externalAudio_;
    double externalDelta_ = 0.0;
    bool externalActive_ = false;    // audio_ è quello esterno
    QString audioNote_;    // es. "audio non disponibile"
    QString decoderText_;  // "Decodifica: GPU (D3D11VA)" / "Decodifica: software"

    double duration_ = 0.0;
    double position_ = 0.0;
    double displayedTime_ = 0.0;      // tempo del fotogramma visibile
    double frameDuration_ = 0.04;
    bool decoderAtDisplayed_ = false; // il decoder è subito dopo il fotogramma visibile
    bool playing_ = false;
    double windowEnd_ = 0.0;  // vedi setPlaybackWindow()

    core::CutSegment segment_;
    std::vector<core::TimeRange> ranges_;  // vedi setRanges()
    double wantedIn_ = 0.0;  // IN come l'ha segnato l'utente, prima dell'aggancio (serve per togliere un tratto)
    bool keyframesKnown_ = false;
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

    // Orologio: audio se c'è, altrimenti tempo reale.
    bool audioActive_ = false;
    bool audioClock_ = false;
    double wallBase_ = 0.0;
    std::chrono::steady_clock::time_point wallStart_;
};

}  // namespace editor::app
