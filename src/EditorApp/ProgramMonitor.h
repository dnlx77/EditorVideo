#pragma once

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QTimer;

namespace editor::core {
class Project;
}

namespace editor::app {

class PlayerWidget;
class TransportBar;

// Anteprima del montaggio: riproduce i pezzi della sequenza uno dopo l'altro. A ogni pezzo apre il file giusto e
// riproduce solo il suo tratto; fra un pezzo e l'altro può esserci una brevissima pausa (il file esportato no).
// Negli spazi vuoti fra i pezzi: nero e silenzio, e il tempo continua a scorrere.
class ProgramMonitor : public QWidget
{
public:
    explicit ProgramMonitor(const core::Project& project, QWidget* parent = nullptr);

    // Da chiamare dopo ogni modifica della sequenza, dei file del progetto o dell'audio (tratti silenziati, traccia
    // sostitutiva).
    void sequenceChanged();
    void seekTo(double seconds);  // tempo della sequenza
    void seekRelative(double deltaSeconds);
    void togglePlayPause();  // mettendo in pausa la velocità torna quella scelta (finisce la spola)
    void setPlaying(bool playing);
    void stop();  // ferma e torna dove era partita la riproduzione
    // Velocità, spola, ripetizione e volume: come in PlayerWidget, per tutto il montaggio (anche negli spazi vuoti e
    // all'indietro da un pezzo al precedente). La ripetizione rifà tutto il montaggio.
    void setBaseSpeed(double speed);
    void shuttle(int direction);
    double speed() const { return speed_; }
    void setLoop(bool loop);
    void setVolume(int percent);
    void setMuted(bool muted);
    void stepFrame(int direction);  // un fotogramma avanti (+1) o indietro (-1), in pausa
    void stepToCut(int direction);  // inizio del pezzo successivo (+1) o di questo / del precedente (-1)
    void seekToEnd();
    // Mostra il fotogramma del file `clipId` al tempo `sourceTime` (es. il bordo di un pezzo mentre lo si trascina),
    // con `note` sotto, senza cambiare la sequenza; seekTo() o sequenceChanged() tornano al montaggio.
    void showSourceFrame(int clipId, double sourceTime, const QString& note);
    bool isPlaying() const { return playing_; }
    double position() const { return position_; }
    int currentIndex() const { return index_; }  // pezzo mostrato (-1 = sequenza vuota o spazio vuoto)
    bool inGap() const { return gapIndex_ >= 0; }  // in uno spazio vuoto
    int currentClipId() const;  // file del pezzo mostrato (0 = nessuno)

    // Proxy (vedi PlayerWidget::videoSourceFor e reloadVideo) e menu della qualità dell'anteprima.
    void setVideoSource(std::function<QString(const QString& path)> source);
    void reloadVideo();
    void setPreviewQuality(int divisor);
    std::function<void(int divisor)> onPreviewQualityChosen;

    std::function<void(double)> onPositionChanged;
    // Tratto già in memoria del pezzo mostrato, nei tempi della sequenza (from == to = niente), per la barretta celeste.
    std::function<void(double from, double to)> onCacheRangeChanged;
    std::function<void()> onActivated;  // l'utente ha usato questa anteprima (per i tasti Spazio e frecce)
    std::function<void()> onPlaybackStarted;
    std::function<void()> onVideoDoubleClicked;  // doppio clic sull'immagine (schermo intero)

    QSize minimumSizeHint() const override { return {120, 60}; }

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void load(int index, double offset);
    void start();     // parte la riproduzione dalla posizione attuale, alla velocità attuale
    void reachedEdge();  // arrivati alla fine (o all'inizio, all'indietro) del montaggio
    void setCurrentSpeed(double speed);
    // Nello spazio vuoto prima del pezzo `index`, al tempo `seconds` della sequenza.
    void enterGap(int index, double seconds);
    void fragmentEnded();
    void updatePosition();
    void updateLabels();

    const core::Project& project_;
    PlayerWidget* player_ = nullptr;
    TransportBar* transport_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* fragmentLabel_ = nullptr;
    QTimer* poll_ = nullptr;

    int index_ = -1;
    int gapIndex_ = -1;     // nello spazio vuoto prima di questo pezzo (-1 = no)
    double gapBase_ = 0.0;  // tempo della sequenza quando è partito gapClock_
    QElapsedTimer gapClock_;
    int loadedClipId_ = 0;  // file aperto nel lettore
    double position_ = 0.0;
    bool playing_ = false;
    double speed_ = 1.0;  // negativa = all'indietro
    double baseSpeed_ = 1.0;
    bool loop_ = false;
    int volume_ = 100;
    bool muted_ = false;
    double playStart_ = 0.0;
    bool hasPlayStart_ = false;
};

}  // namespace editor::app
