#pragma once

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/SequenceAudio.h"
#include "EditorApp/TextOverlay.h"
#include "EditorCore/LayerPlayback.h"
#include "EditorCore/TextTracks.h"
#include "EditorCore/Transform.h"
#include "EditorCore/TransformManip.h"
#include "EditorCore/TransitionPlayback.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

class QToolButton;

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QTimer;

namespace editor::core {
class Project;
struct VideoFrame;
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
    ~ProgramMonitor() override;

    // Da chiamare dopo ogni modifica della sequenza, dei file del progetto o dell'audio (tratti silenziati, traccia
    // sostitutiva).
    void sequenceChanged();
    // Livello, muto o solo di una traccia audio cambiati: si sente subito, senza fermare la riproduzione.
    void audioMixChanged();
    void seekTo(double seconds);  // tempo della sequenza
    double programEnd() const;  // fine del programma: ultimo pezzo, scritta o pezzo di livello
    bool hasProgram() const;  // c'è qualcosa da riprodurre: pezzi su V1, oppure solo scritte e livelli
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
    // Nel tratto di una transizione (id, 0 = no): il monitor mostra i due pezzi fusi, non un file solo. Il fotogramma
    // mostrato per ultimo (nullptr se non c'è) serve alle prove.
    int transitionShown() const { return transitionId_; }
    std::shared_ptr<core::VideoFrame> transitionFrame() const { return transitionFrame_; }
    int currentClipId() const;  // file del pezzo mostrato (0 = nessuno)
    // Le scritte che si vedono ora sopra l'immagine (nessuna mentre si mostra un fotogramma di un file con showSourceFrame).
    std::vector<core::TextClip> visibleText() const;
    const TextOverlay& textOverlay() const { return overlay_; }
    // Quanti pezzi di livelli sopra V1 si vedono ora con il loro fotogramma (per le prove).
    int layerFramesShown() const { return layerFramesShown_; }

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
    // Scritte trascinate sull'immagine (da fermo): afferrata (per sceglierla) e lasciata con il nuovo centro (0..1).
    std::function<void(int clipId)> onTextGrabbed;
    std::function<void(int clipId, double centerX, double centerY)> onTextDragged;

    // Manipolatori sul monitor (tasto Q / pulsante «Trasforma»): sul pezzo mostrato, a monitor fermo, si trascina il centro
    // (posizione), un angolo (scala; Maiusc = proporzioni libere, Alt = dal centro) o la maniglia sopra il riquadro
    // (rotazione; Maiusc = scatti di 15°). `onTransformEdited` arriva una volta sola, al rilascio, con il pezzo e la nuova
    // trasformazione (un gesto = un passo di Annulla).
    void setTransformMode(bool on);
    bool transformMode() const { return transformMode_; }
    std::function<void(int fragmentId, const core::Transform& transform)> onTransformEdited;
    // Il pezzo di un livello (V2, V3...) scelto sulla linea del tempo (0 = nessuno): le maniglie agiscono su quello, se è
    // sotto il cursore. Il gesto finito arriva a `onLayerTransformEdited`.
    std::function<int()> selectedLayerPiece;
    std::function<void(int pieceId, const core::Transform& transform)> onLayerTransformEdited;
    void refreshManipulators();  // la scelta sulla linea del tempo è cambiata
    std::function<void(bool on)> onTransformModeChanged;  // cambiato dal pulsante del monitor
    // Ritaglio sul monitor (tasto W): maniglie sui quattro lati e sugli angoli del riquadro ritagliato; il lato va dove porti il
    // mouse. Si esclude con la modalità Trasforma.
    void setCropMode(bool on);
    bool cropMode() const { return cropMode_; }
    std::function<void(bool on)> onCropModeChanged;  // cambiato dal pulsante del monitor o da Trasforma
    // La griglia dei terzi sopra l'immagine (sempre, anche senza manipolatori).
    void setThirdsGrid(bool on);
    bool thirdsGrid() const { return thirdsGrid_; }
    // Per le prove: i manipolatori sono visibili e sul pezzo (non in riproduzione, né in una transizione o in uno spazio vuoto).
    bool manipulatorsVisible() const;

    QSize minimumSizeHint() const override { return {120, 60}; }

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void load(int index, double offset);
    // L'audio del montaggio: il mix della sequenza dal punto `from` (secondi della sequenza), alla velocità attuale.
    // false se a questa velocità non si sente o non c'è una scheda audio: la riproduzione va a tempo reale.
    bool startAudio(double from);
    void stopAudio();
    void applyVolume();
    void start();     // parte la riproduzione dalla posizione attuale, alla velocità attuale
    void reachedEdge();  // arrivati alla fine (o all'inizio, all'indietro) del montaggio
    void setCurrentSpeed(double speed);
    // Nello spazio vuoto prima del pezzo `index`, al tempo `seconds` della sequenza.
    void enterGap(int index, double seconds);
    void fragmentEnded();
    void updatePosition();
    void updateLabels();
    QSize frameSize() const;
    bool grabImage(QPointF at);
    bool grabText(QPointF at);
    // Manipolatori: la trasformazione del pezzo mostrato (quella in corso se si sta trascinando), il suo disegno e il gesto.
    const core::LayerPiece* manipPiece() const;
    core::Transform v1Transform() const;  // la trasformazione del pezzo di V1 mostrato (quella in corso se lo si trascina)
    void showDraggedLayer();
    core::Transform shownTransform() const;
    std::optional<core::ManipSize> manipSize() const;  // misure del file e del progetto; vuoto se non c'è da manipolare
    double manipScreenScale() const;                   // pixel dello schermo per pixel del progetto
    void paintManipulators(QPainter& painter, const QRectF& target);
    void dragImage(QPointF at);
    void dropImage(QPointF at);
    void applyShownTransform();
    void dragText(QPointF at);
    void dropText(QPointF at);
    void updateOverlayFrame();  // dice al lettore come disegnare le scritte
    void updateTextOverlay();  // ridisegna le scritte se quelle visibili sono cambiate
    // I pezzi di video dei livelli sopra V1 che si vedono al tempo attuale: ognuno ha la sua decodifica in un thread a parte
    // (core::LayerPlayback), aperta un attimo prima che il pezzo inizi; il lettore li disegna sopra V1.
    void updateLayers();
    // Transizioni: la transizione in cui cade `seconds` (non l'ultimo istante, che è già del pezzo dopo).
    std::optional<core::TransitionPlacement> transitionAt(double seconds) const;
    // `wait`: aspetta il fotogramma (cursore fermo); altrimenti parte il flusso di fotogrammi preparati in anticipo, che un
    // timer veloce mostra al tempo dell'audio (riproduzione).
    void enterTransition(const core::TransitionPlacement& placement, double seconds, bool wait);
    void showTransitionFrame(const core::TransitionPlacement& placement, double seconds, bool wait);
    void primeTransition();  // in riproduzione: apre in anticipo i file della transizione che sta per arrivare
    void setTransitionBox();
    double transitionClock() const;  // il tempo della sequenza adesso, nel tratto di una transizione in riproduzione
    void transitionTick();           // mostra il fotogramma pronto per questo momento

    const core::Project& project_;
    TextOverlay overlay_;
    std::vector<core::TextClip> shownText_;  // le scritte disegnate l'ultima volta
    double shownTime_ = 0.0;                 // e il tempo del montaggio di allora (per le dissolvenze)
    bool shownFading_ = false;               // una di loro era in dissolvenza
    struct TextDrag
    {
        int index = -1;       // posto della scritta in shownText_
        QPointF grabOffset;   // dal centro della scritta al punto afferrato (frazioni del fotogramma)
    };
    std::optional<TextDrag> textDrag_;
    struct ManipDrag
    {
        int fragmentId = 0;
        core::ManipHit hit;
        core::Transform start;
        core::Transform current;
        core::Point2 from;  // dove si è afferrato, in pixel del progetto
        int cropEdges = 0;  // in modalità Ritaglia: i lati trascinati (kCropLeft...)
        bool layer = false;  // `fragmentId` è un pezzo di un livello, non di V1
    };
    std::optional<ManipDrag> manipDrag_;
    bool transformMode_ = false;
    bool cropMode_ = false;
    bool thirdsGrid_ = false;
    core::SnapGuides snapGuides_;  // le guide su cui sta scattando il gesto in corso
    bool rotationSnapped_ = false;
    QToolButton* transformButton_ = nullptr;
    QToolButton* cropButton_ = nullptr;
    bool showingSource_ = false;             // si mostra un fotogramma di un file, non il montaggio
    PlayerWidget* player_ = nullptr;
    SequenceAudio sequenceAudio_;
    TransportBar* transport_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* fragmentLabel_ = nullptr;
    QTimer* poll_ = nullptr;

    int index_ = -1;
    int gapIndex_ = -1;     // nello spazio vuoto prima di questo pezzo (-1 = no)
    int transitionId_ = 0;  // nel tratto di questa transizione (0 = no); il tempo scorre come negli spazi vuoti
    core::TransitionPlayback transitionPlayback_;  // fa i fotogrammi in un thread a parte
    QTimer* transitionTimer_ = nullptr;            // in riproduzione nella transizione: sceglie il fotogramma ogni pochi ms
    double transitionFrom_ = 0.0;                  // tratto della transizione in cui si sta (tempi della sequenza)
    double transitionTo_ = 0.0;
    int primedTransitionId_ = 0;                   // quella aperta in anticipo
    std::shared_ptr<core::VideoFrame> transitionFrame_;
    double gapBase_ = 0.0;  // tempo della sequenza quando è partito gapClock_
    QElapsedTimer gapClock_;
    int loadedClipId_ = 0;  // file aperto nel lettore
    std::map<int, std::unique_ptr<core::LayerPlayback>> layerPlaybacks_;  // per id del pezzo del livello
    std::vector<PlayerWidget::LayerFrame> lastLayers_;  // i fotogrammi dei livelli mostrati, e di chi sono
    std::vector<int> lastLayerIds_;
    int layerFramesShown_ = 0;
    bool layersShown_ = false;  // ci sono pezzi di livelli sopra V1: V1 si disegna sempre nel fotogramma del progetto
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
