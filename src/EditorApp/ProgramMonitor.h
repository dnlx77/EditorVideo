#pragma once

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QTimer;
class QToolButton;

namespace editor::core {
class Project;
}

namespace editor::app {

class PlayerWidget;

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
    void togglePlayPause();
    void setPlaying(bool playing);  // L riproduce, K mette in pausa
    void stepFrame(int direction);  // un fotogramma avanti (+1) o indietro (-1), in pausa
    void stepToCut(int direction);  // inizio del pezzo successivo (+1) o di questo / del precedente (-1)
    void seekToEnd();
    bool isPlaying() const { return playing_; }
    double position() const { return position_; }
    int currentIndex() const { return index_; }  // pezzo mostrato (-1 = sequenza vuota o spazio vuoto)
    bool inGap() const { return gapIndex_ >= 0; }  // in uno spazio vuoto

    std::function<void(double)> onPositionChanged;
    std::function<void()> onActivated;  // l'utente ha usato questa anteprima (per i tasti Spazio e frecce)
    std::function<void()> onPlaybackStarted;
    std::function<void()> onVideoDoubleClicked;  // doppio clic sull'immagine (schermo intero)

    QSize minimumSizeHint() const override { return {120, 60}; }

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void load(int index, double offset);
    // Nello spazio vuoto prima del pezzo `index`, al tempo `seconds` della sequenza.
    void enterGap(int index, double seconds);
    void fragmentEnded();
    void updatePosition();
    void updateLabels();

    const core::Project& project_;
    PlayerWidget* player_ = nullptr;
    QToolButton* playButton_ = nullptr;
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
};

}  // namespace editor::app
