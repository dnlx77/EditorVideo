#pragma once

#include "EditorApp/AudioOutput.h"

#include <QtWidgets/QWidget>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

class QLabel;
class QTimer;
class QToolButton;

namespace editor::core {
class AudioDecoder;
class VideoDecoder;
struct VideoFrame;
}  // namespace editor::core

namespace editor::app {

class SeekSlider;
class VideoView;

// Anteprima con riproduzione audio/video sincronizzata, barra di posizione e spostamento in un punto.
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
    void seekTo(double seconds);
    void seekRelative(double deltaSeconds);

    bool isPlaying() const { return playing_; }
    double currentPosition();  // secondi
    QString statusText() const;  // es. "Decodifica: GPU (D3D11VA)"

private:
    void play();
    void pause();
    void startWorkers(double from);
    void stopWorkers();
    void videoWorker();
    void audioWorker();

    double clock();
    void tick();
    void showFrameAt(double seconds);
    void updateControls();
    void updatePositionLabel(double seconds);

    VideoView* view_ = nullptr;
    QToolButton* playButton_ = nullptr;
    SeekSlider* slider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* decoderLabel_ = nullptr;
    QTimer* timer_ = nullptr;
    QTimer* resizeRefresh_ = nullptr;

    std::unique_ptr<core::VideoDecoder> video_;
    std::unique_ptr<core::AudioDecoder> audio_;
    AudioOutput audioOut_;
    QString audioNote_;    // es. "audio non disponibile"
    QString decoderText_;  // "Decodifica: GPU (D3D11VA)" / "Decodifica: software"

    double duration_ = 0.0;
    double position_ = 0.0;
    bool playing_ = false;

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

    // Orologio: audio se c'è, altrimenti tempo reale.
    bool audioActive_ = false;
    bool audioClock_ = false;
    double wallBase_ = 0.0;
    std::chrono::steady_clock::time_point wallStart_;
};

}  // namespace editor::app
