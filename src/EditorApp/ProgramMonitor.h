#pragma once

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/SequenceAudio.h"
#include "EditorCore/Spectrum.h"
#include "EditorApp/TextOverlay.h"
#include "EditorCore/LayerPlayback.h"
#include "EditorCore/TextTracks.h"
#include "EditorCore/Transform.h"
#include "EditorCore/TransformManip.h"
#include "EditorCore/TransitionPlayback.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

class QToolButton;

#include <array>
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
    // Il guadagno di «Normalizza» sul Master misurato sul mix intero (1 = nessuno); si moltiplica al livello del Master.
    void setNormalizeGain(float gain);
    // I picchi di ogni traccia audio dall'ultima chiamata, per i vu-meter del Mixer (vuoto se l'audio non suona).
    std::map<int, std::array<float, 2>> takeTrackPeaks() { return sequenceAudio_.takePeaks(); }
    // Lo spettro (dB, core::spectrumDb) di una traccia o del Master mentre suona; vuoto se non suona.
    void setSpectrumWatch(int trackId) { sequenceAudio_.setSpectrumWatch(trackId); }
    std::vector<float> takeSpectrum()
    {
        std::vector<float> samples;
        if (!sequenceAudio_.takeSpectrumSamples(samples, core::kSpectrumSize))
            return {};
        return core::spectrumDb(samples);
    }
    void seekTo(double seconds);  // tempo della sequenza
    double programEnd() const;  // fine del programma: ultimo pezzo, scritta, pezzo di livello o clip audio
    bool hasProgram() const;  // c'è qualcosa da riprodurre: pezzi su V1, oppure solo scritte, livelli o clip audio
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
    int currentClipId() const;  // file del pezzo mostrato (0 = nessuno)
    // Le scritte che si vedono ora sopra l'immagine (nessuna mentre si mostra un fotogramma di un file con showSourceFrame).
    std::vector<core::TextClip> visibleText() const;

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
    std::function<void(int fragmentId, const core::Transform& transform)> onTransformEdited;
    // Il pezzo di un livello (V2, V3...) scelto sulla linea del tempo (0 = nessuno): le maniglie agiscono su quello, se è
    // sotto il cursore. Il gesto finito arriva a `onLayerTransformEdited`.
    std::function<int()> selectedLayerPiece;
    std::function<void(int pieceId, const core::Transform& transform)> onLayerTransformEdited;
    // Una scritta scelta da sola sulla linea del tempo (0 = nessuna): se è sotto il cursore le maniglie agiscono su di lei (scala 100% =
    // la grandezza della scritta; ruota e scala attorno al suo centro di stile). Il gesto finito arriva a `onTextTransformEdited`,
    // un punto del percorso a `onTextMotionKeyMoved` (istante in secondi dall'inizio della scritta, spostamento `posX/posY`).
    std::function<int()> selectedText;
    std::function<void(int clipId, const core::Transform& transform)> onTextTransformEdited;
    std::function<void(int clipId, double clipTime, double posX, double posY)> onTextMotionKeyMoved;
    // Percorso del movimento: con la posizione animata (almeno due istanti) e il tasto dei manipolatori acceso, sul monitor si
    // vede la linea che il centro del pezzo percorre, con un punto per ogni chiave. Un punto si trascina: al rilascio arriva qui
    // l'istante della chiave (nel file) e la nuova posizione (spostamento dal centro del fotogramma, in pixel del progetto).
    std::function<void(bool layer, int id, double fileTime, double posX, double posY)> onMotionKeyMoved;
    struct MotionDot
    {
        double fileTime = 0.0;
        core::Point2 pos;  // il centro del pezzo, in pixel del progetto (origine in alto a sinistra)
    };
    struct MotionPath
    {
        bool layer = false;
        bool text = false;  // di una scritta: gli istanti sono secondi dall'inizio della scritta
        int id = 0;
        std::vector<MotionDot> dots;
        std::vector<core::Point2> line;  // la curva, campionata
    };
    std::optional<MotionPath> motionPath() const;
    void refreshManipulators();  // la scelta sulla linea del tempo è cambiata
    std::function<void(bool on)> onTransformModeChanged;  // cambiato dal pulsante del monitor
    // Ritaglio sul monitor (tasto W): maniglie sui quattro lati e sugli angoli del riquadro ritagliato; il lato va dove porti il
    // mouse. Si esclude con la modalità Trasforma.
    void setCropMode(bool on);
    std::function<void(bool on)> onCropModeChanged;  // cambiato dal pulsante del monitor o da Trasforma
    // La griglia dei terzi sopra l'immagine (sempre, anche senza manipolatori).
    void setThirdsGrid(bool on);
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
    // La scritta scelta, se è sotto il cursore (e le maniglie non sono quelle del ritaglio, che le scritte non hanno).
    const core::TextClip* manipText() const;
    // Una scritta si manipola come un file «virtuale» grande quanto il suo riquadro: scala 1 = grandezza della scritta, e il
    // centro del riquadro (non il punto di stile attorno a cui ruota davvero) è il centro dell'immagine. toVirtual/toReal
    // passano dalla trasformazione vera alla virtuale e viceversa.
    struct TextManip
    {
        const core::TextClip* clip = nullptr;
        core::Point2 pivot;      // il centro di stile, in pixel del progetto
        core::Point2 boxCenter;  // il centro del riquadro della scritta
        double fit = 1.0;        // il fattore «adatta al fotogramma» del riquadro
        core::ManipSize size;    // il riquadro e il progetto
    };
    std::optional<TextManip> textManip() const;
    core::Transform textToVirtual(const core::Transform& real, const TextManip& m) const;
    core::Transform textToReal(const core::Transform& virt, const TextManip& m) const;
    core::Transform textShownReal() const;  // la trasformazione della scritta al tempo del cursore
    void showDraggedText();
    mutable core::TextClip boxClip_;  // l'ultima scritta di cui si è misurato il riquadro (senza trasformazione)
    mutable QSize boxFrame_;
    mutable QRectF boxRect_;
    core::Transform v1Transform() const;  // la trasformazione del pezzo di V1 mostrato (quella in corso se lo si trascina)
    void showDraggedLayer();
    core::Transform layerPieceTransform(const core::LayerPiece& piece) const;  // con i fotogrammi chiave, al tempo del cursore
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
    std::vector<int> shownLevels_;  // il piano di ogni scritta di shownText_ (vedi TextTracks::levelOf)
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
        bool text = false;   // `fragmentId` è una scritta (le trasformazioni sono quelle «virtuali», vedi TextManip)
    };
    std::optional<ManipDrag> manipDrag_;
    struct MotionDrag
    {
        MotionPath path;
        std::size_t dot = 0;
        core::Point2 current;
    };
    std::optional<MotionDrag> motionDrag_;
    int motionDotAt(const MotionPath& path, core::Point2 p, double screenScale) const;  // -1 = nessuno
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
    float normalizeGain_ = 1.0f;  // vedi setNormalizeGain
    bool muted_ = false;
    double playStart_ = 0.0;
    bool hasPlayStart_ = false;

private:
    // Solo per le prove (EditorVideoTests.exe e --smoke-test), che li raggiungono attraverso TestAccess (TestAccess.h).
    friend class TestAccess;
    bool inGap() const { return gapIndex_ >= 0; }  // in uno spazio vuoto
    // Nel tratto di una transizione (id, 0 = no): il monitor mostra i due pezzi fusi, non un file solo; il fotogramma
    // mostrato per ultimo (nullptr se non c'è).
    int transitionShown() const { return transitionId_; }
    std::shared_ptr<core::VideoFrame> transitionFrame() const { return transitionFrame_; }
    const TextOverlay& textOverlay() const { return overlay_; }
    // Quanti pezzi di livelli sopra V1 si vedono ora con il loro fotogramma.
    int layerFramesShown() const { return layerFramesShown_; }
    bool transformMode() const { return transformMode_; }
    bool cropMode() const { return cropMode_; }
    bool thirdsGrid() const { return thirdsGrid_; }
    // I quattro angoli del riquadro mostrato, in pixel del progetto (vuoto se le maniglie non si vedono).
    std::optional<std::array<core::Point2, 4>> manipQuad() const;
};

}  // namespace editor::app
