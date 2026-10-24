#pragma once

#include "EditorApp/ZoomableTimeline.h"
#include "EditorCore/AudioTracks.h"
#include "EditorCore/TimeUtil.h"

#include <QtCore/QStringList>
#include <QtGui/QImage>
#include <QtGui/QPixmap>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QMimeData;
class QToolButton;

namespace editor::core {
struct Waveform;
}

namespace editor::app {

// Linea del tempo del montaggio: una corsia video (V1) e sotto le corsie audio (A1 = l'audio dei pezzi video, poi le
// tracce audio aggiunte A2, A3...). A sinistra di ogni corsia il nome e, per l'audio, i pulsanti muto (M) e solo (S) e
// il livello; in alto a sinistra i pulsanti "+" e "−" aggiungono e tolgono una traccia audio.
// Video: un blocco colorato per ogni pezzo della sequenza (un colore per file), largo quanto dura, con il nome del
// file. Clic = seleziona e sposta l'anteprima del montaggio lì; trascinamento = sposta il pezzo avanti o indietro
// (da solo: gli altri restano dove sono, e fra i pezzi restano spazi vuoti); doppio clic = riapre il pezzo
// nell'anteprima di partenza. Clic su uno spazio vuoto = lo seleziona (Canc lo chiude).
// A1: la forma d'onda di ogni pezzo, i tratti senza audio barrati. Tracce aggiunte: le clip audio, che si spostano
// trascinandole (anche su un'altra traccia, in verticale) e si accorciano dai bordi come i pezzi video. Trascinando
// sullo sfondo di una corsia audio si seleziona un tratto.
// Si può ingrandire (vedi ZoomableTimeline).
class SequenceTimeline : public ZoomableTimeline
{
public:
    struct Block
    {
        int fragmentId = 0;
        int colorIndex = 0;  // di solito la posizione del file nel progetto
        QString label;       // es. "vacanze.mp4"
        double length = 0.0;
        double sourceStart = 0.0;  // inizio del pezzo nel file (per la forma d'onda e le miniature)
        int clipId = 0;            // file di origine (per le miniature)
        std::shared_ptr<const core::Waveform> waveform;  // nullptr = in calcolo
        double gap = 0.0;          // spazio vuoto prima del pezzo
        // Come entra nel file esportato (valori di core::ClipFit): 0 copia, 1 copia con i suoi parametri, 2 da
        // ricodificare. Con 1 e 2 un segno sul blocco, e fitNote nel suggerimento.
        int fit = 0;
        QString fitNote;
        int proxyDivisor = 1;  // l'anteprima legge il video da un proxy a 1/proxyDivisor (1 = dall'originale)
    };

    // Una clip audio su una traccia aggiunta.
    struct AudioBlock
    {
        int clipId = 0;
        QString path;    // il file (per la forma d'onda)
        QString label;   // es. "musica.mp3"
        int colorIndex = 0;
        double position = 0.0;     // dove inizia nella sequenza
        double length = 0.0;
        double sourceStart = 0.0;  // inizio nel file
        std::shared_ptr<const core::Waveform> waveform;  // nullptr = in calcolo
    };

    // Una corsia audio: A1 (trackId 0: le sue forme d'onda sono quelle dei pezzi video, `clips` non conta) o una
    // traccia aggiunta.
    struct AudioLane
    {
        int trackId = 0;
        QString name;  // "A1", "A2"...
        double levelDb = 0.0;
        bool muted = false;
        bool solo = false;
        bool audible = true;  // si sente nel mix (muto e solo di tutte le tracce compresi)
        std::vector<AudioBlock> clips;
        std::vector<core::LevelPoint> envelope;  // la linea del livello
    };

    explicit SequenceTimeline(QWidget* parent = nullptr);

    void setBlocks(std::vector<Block> blocks);
    void setPlayhead(double seconds);
    void setSelected(int fragmentId);
    int selected() const { return selected_; }
    // Spazio vuoto selezionato: quello prima del pezzo `fragmentId` (0 = nessuno). Selezionare uno spazio toglie la
    // selezione del pezzo e viceversa.
    void setSelectedGap(int fragmentId);
    int selectedGap() const { return selectedGap_; }

    void setMuted(std::vector<core::TimeRange> ranges);  // tratti senza audio di A1, nei tempi della sequenza
    // Corsie audio: la prima è sempre A1. Se c'erano più corsie (o meno) il widget cambia altezza.
    void setAudioLanes(std::vector<AudioLane> lanes);
    const std::vector<AudioLane>& audioLanes() const { return lanes_; }
    // Quando finisce una forma d'onda: aggiorna solo quelle dei blocchi (`forClip` dà quella di un file del progetto) e
    // delle clip audio (`forPath`), senza rifare il resto.
    void setWaveforms(const std::function<std::shared_ptr<const core::Waveform>(int clipId)>& forClip,
                      const std::function<std::shared_ptr<const core::Waveform>(const QString& path)>& forPath);
    // Tratto selezionato su una corsia audio (tempi della sequenza; `trackId` 0 = A1); to <= from = nessuno.
    void setAudioSelection(double from, double to, int trackId = 0);
    bool hasAudioSelection() const { return selectionTo_ > selectionFrom_; }
    double audioSelectionFrom() const { return selectionFrom_; }
    double audioSelectionTo() const { return selectionTo_; }
    int audioSelectionTrack() const { return selectionTrack_; }
    // Clip audio selezionata (0 = nessuna): selezionarla toglie la selezione del pezzo video e viceversa.
    void setSelectedAudioClip(int clipId);
    int selectedAudioClip() const { return selectedAudioClip_; }
    // Traccia audio scelta (clic sul suo nome o su una sua clip): quella che toglie il pulsante "−". 0 = A1.
    void setSelectedTrack(int trackId);
    int selectedTrack() const { return selectedTrack_; }

    std::function<void(double)> onSeek;               // clic: tempo della sequenza
    std::function<void(int)> onSelect;                // pezzo selezionato
    // Trascinamento di un pezzo: dove può andare se lo si vuole far iniziare a `desired` (vedi
    // Sequence::nearestFreeStart), e dove è stato lasciato.
    std::function<double(int fragmentId, double desired)> freeStart;
    std::function<void(int fragmentId, double start)> onMoveTo;
    // Bordi dei pezzi: vicino al bordo sinistro o destro di un blocco il cursore diventa ⟷ e trascinando si sposta
    // l'inizio (atStart) o la fine del pezzo nel file (Alt premuto = ripple: i pezzi dopo scorrono). trimLimit dice
    // dove può andare il bordo se lo si vuole a `desired` (tempo nel file); onTrimPreview si chiama durante il
    // trascinamento con il nuovo bordo, onTrim quando lo si lascia.
    std::function<double(int fragmentId, bool atStart, double desired, bool ripple)> trimLimit;
    std::function<void(int fragmentId, bool atStart, double value)> onTrimPreview;
    std::function<void(int fragmentId, bool atStart, double value, bool ripple)> onTrim;
    std::function<void(int)> onActivate;              // doppio clic
    std::function<void(double from, double to)> onAudioSelection;  // tratto selezionato (vuoto = tolto)
    // File trascinati da Esplora file e lasciati qui: vanno nella sequenza a partire dalla posizione `index`.
    std::function<void(const QStringList& paths, int index)> onFilesDropped;

    // Tracce audio: pulsanti "+" e "−" in alto a sinistra, muto, solo e livello di una corsia (onLevel si chiama
    // lasciando il cursore del livello, o con un doppio clic che lo riporta a 0 dB).
    std::function<void()> onAddTrack;
    std::function<void(int trackId)> onRemoveTrack;
    std::function<void(int trackId, bool muted)> onTrackMuted;
    std::function<void(int trackId, bool solo)> onTrackSolo;
    std::function<void(int trackId, double db)> onTrackLevel;
    // Linea del livello (al centro della corsia): il clic sulla linea aggiunge un punto, trascinandolo si cambiano
    // tempo e livello (vicino a 0 dB si aggancia al centro), il doppio clic su un punto lo toglie. Lasciando il mouse si
    // chiama onEnvelopeChanged con la linea nuova.
    std::function<void(int trackId, const std::vector<core::LevelPoint>& points)> onEnvelopeChanged;
    // Clip audio: selezione, spostamento (dove può andare sulla traccia `trackId` se la si vuole a `desired`, e dove è
    // stata lasciata), bordi (come trimLimit/onTrim dei pezzi video, nei tempi del file della clip).
    std::function<void(int clipId)> onAudioClipSelected;
    std::function<double(int clipId, int trackId, double desired)> audioFreePosition;
    std::function<void(int clipId, int trackId, double position)> onAudioClipMoveTo;
    std::function<double(int clipId, bool atStart, double desired, bool ripple)> audioTrimLimit;
    std::function<void(int clipId, bool atStart, double value, bool ripple)> onAudioTrim;
    // File trascinati su una traccia aggiunta: vanno su quella traccia a partire da `seconds` della sequenza.
    std::function<void(const QStringList& paths, int trackId, double seconds)> onAudioFilesDropped;
    // Miniatura del file `clipId` al tempo `sourceTime` (nei tempi del file); un'immagine vuota se non è ancora
    // pronta (chi la fornisce la prepara in background e poi chiede di ridisegnare).
    std::function<QImage(int clipId, double sourceTime)> thumbnailFor;
    // Da chiamare quando thumbnailFor ha nuove miniature pronte: si ridisegnano i blocchi.
    void thumbnailsChanged();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Solo per le prove: dove sta un tempo della sequenza e il centro della corsia video, in coordinate del widget.
    double xForSequenceTime(double seconds) const { return xForTime(seconds); }
    double videoLaneCenter() const { return videoLane().center().y(); }
    double audioLaneCenter(int laneIndex) const { return audioLane(laneIndex).center().y(); }  // 0 = A1
    QToolButton* addTrackButton() const { return addTrack_; }
    QToolButton* removeTrackButton() const { return removeTrack_; }
    double xForBlockEdge(int index, bool atStart) const;  // dove sta il bordo del blocco `index`

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    QRectF timeAxis() const override { return videoLane(); }
    void viewChanged() override { invalidate(); }
    double playheadTime() const override { return playhead_; }

private:
    // Tutto tranne il cursore (blocchi, miniature, forme d'onda, tratti) si disegna in cache_ e si rifà solo quando
    // cambia; a ogni ridisegno si copia cache_ e sopra si disegna il cursore.
    void invalidate();
    void paintStatic(QPainter& p);
    QRect cursorRect(double seconds) const;
    double videoHeight() const;
    QRectF videoLane() const;
    QRectF audioLane(int index = 0) const;  // corsia audio `index` (0 = A1)
    int laneAt(double y) const;             // corsia audio sotto y (-1 = nessuna)
    QRectF headerRect(const QRectF& lane) const;  // il riquadro a sinistra della corsia, con nome e pulsanti
    // Pulsanti di una corsia audio nel suo riquadro a sinistra: muto, solo, livello.
    QRectF muteBox(int index) const;
    QRectF soloBox(int index) const;
    QRectF levelBar(int index) const;
    // Linea del livello: dove sta un valore (dB) nella corsia e il contrario. Sopra il centro 0..+12 dB, sotto 0..-60
    // (con più precisione vicino a 0).
    double envelopeY(const QRectF& lane, double db) const;
    double envelopeDbForY(const QRectF& lane, double y) const;
    // La linea mostrata per la corsia: quella che si sta trascinando, o quella del progetto.
    const std::vector<core::LevelPoint>& lineOf(int laneIndex) const;
    // Punto della linea (point >= 0) o tratto di linea (point = -1) sotto il mouse; false se nessuno dei due.
    bool envelopeHit(const QPointF& pos, int& laneIndex, int& point) const;
    void paintEnvelope(QPainter& p, int index, const QRectF& lane);
    double levelForX(int index, double x) const;  // dB del cursore del livello quando il mouse è in x
    std::vector<QRectF> blockRects(const QRectF& lane) const;  // il riquadro di ogni blocco nella corsia
    double total() const { return total_; }
    // La parte visibile di `r` (con un po' di margine): con lo zoom un blocco può essere largo milioni di pixel.
    QRectF visiblePart(const QRectF& r) const;
    int blockAt(double x) const;     // indice del blocco sotto x, -1 se nessuno (o in uno spazio vuoto)
    int gapAt(double x) const;       // indice del blocco che ha subito prima lo spazio vuoto sotto x, -1 se nessuno
    int dropIndexAt(double x) const;  // dove finirebbero i file trascinati
    double startOfBlock(int index) const;  // dove inizia il blocco nella sequenza
    // Inizio del blocco trascinato quando il mouse è in x: agganciato (se vicino) ai bordi degli altri blocchi e
    // all'inizio, poi spostato dove c'è posto (freeStart).
    double dragTarget(double x) const;
    // Quale corsia è quella scelta (evidenziata): la video se è scelto un pezzo o uno spazio vuoto, l'audio della
    // traccia scelta (nome, clip o tratto).
    bool videoLaneSelected() const;
    bool audioLaneSelected(int index) const;
    void paintAudio(QPainter& p);
    void paintVideo(QPainter& p);
    void paintA1(QPainter& p, const QRectF& lane);
    void paintTrack(QPainter& p, int index, const QRectF& lane);
    void paintHeaders(QPainter& p);
    // Clip audio sotto il punto: indice della corsia e della clip (false se nessuna).
    bool audioClipAt(const QPointF& pos, int& lane, int& clip) const;
    // Bordo di una clip audio sotto il punto (entro pochi pixel): come edgeAt, per le tracce aggiunte.
    bool audioEdgeAt(const QPointF& pos, int& lane, int& clip, bool& atStart) const;
    const AudioBlock* findAudioClip(int clipId, int* lane = nullptr) const;
    // Inizio della clip audio trascinata quando il mouse è in x, sulla corsia `lane` (agganciata ai bordi e al cursore).
    double audioDragTarget(double x, int lane) const;
    double audioTrimTarget(double x, bool ripple) const;
    // Agganci magnetici: inizio della sequenza, cursore, bordi dei pezzi video e delle clip audio (tranne `ignoredClip`).
    std::vector<double> snapMarks(int ignoredClip) const;
    void syncTrackButtons();
    void updateDuration();  // durata mostrata: fino alla fine dei pezzi o delle clip audio, più un po' di posto
    // Bordo di un blocco sotto il punto (corsia video, entro pochi pixel): indice del blocco, -1 se nessuno; atStart
    // dice se è l'inizio o la fine.
    int edgeAt(const QPointF& pos, bool& atStart) const;
    // Tempo nel file del bordo trascinato quando il mouse è in x (agganciato al cursore e ai bordi degli altri blocchi,
    // poi limitato da trimLimit).
    double trimTarget(double x, bool ripple) const;
    double trimEdgeSequenceTime(double value) const;  // dove cade nella sequenza il bordo trascinato a `value`

    std::vector<Block> blocks_;
    double total_ = 0.0;  // durata della sequenza: somma dei blocchi, calcolata in setBlocks
    std::vector<double> starts_;  // inizio di ogni blocco (dopo il suo spazio vuoto), calcolato in setBlocks
    std::vector<core::TimeRange> muted_;
    std::vector<AudioLane> lanes_{AudioLane{0, QStringLiteral("A1")}};
    double playhead_ = 0.0;
    int selected_ = 0;
    int selectedGap_ = 0;
    int dragIndex_ = -1;      // blocco che si sta trascinando
    double pressX_ = 0.0;
    double pressY_ = 0.0;
    bool dragging_ = false;
    double dragGrab_ = 0.0;   // distanza (in secondi) fra l'inizio del blocco e il punto in cui lo si è preso
    double dragStart_ = 0.0;  // dove andrà il blocco trascinato se lo si lascia adesso

    int trimIndex_ = -1;       // blocco di cui si trascina un bordo
    bool trimAtStart_ = false;
    bool trimming_ = false;    // il bordo si è mosso
    bool trimRipple_ = false;
    double trimValue_ = 0.0;   // nuovo bordo, tempo nel file
    double trimGrab_ = 0.0;    // distanza (in secondi) fra il bordo e il punto in cui lo si è preso

    double selectionFrom_ = 0.0;
    double selectionTo_ = 0.0;
    int selectionTrack_ = 0;
    bool selecting_ = false;     // si sta trascinando una selezione su una corsia audio
    double selectionAnchor_ = 0.0;
    bool scrubbing_ = false;     // si sta trascinando il cursore sul righello

    int selectedAudioClip_ = 0;
    int selectedTrack_ = -1;      // -1 = nessuna scelta
    int audioDragClip_ = 0;       // clip audio presa (spostamento)
    bool audioDragging_ = false;
    double audioDragGrab_ = 0.0;
    double audioDragStart_ = 0.0;  // dove andrà
    int audioDragLane_ = -1;       // su quale corsia
    int audioTrimClip_ = 0;        // clip audio di cui si trascina un bordo
    bool audioTrimAtStart_ = false;
    bool audioTrimming_ = false;
    bool audioTrimRipple_ = false;
    double audioTrimValue_ = 0.0;
    double audioTrimGrab_ = 0.0;
    int envLane_ = -1;             // corsia di cui si modifica la linea del livello
    int envPoint_ = -1;            // punto preso
    bool envMoved_ = false;        // la linea è cambiata (punto aggiunto o spostato)
    std::vector<core::LevelPoint> envPoints_;  // la linea in modifica
    int levelLane_ = -1;           // corsia di cui si trascina il livello
    double levelValue_ = 0.0;

    QToolButton* addTrack_ = nullptr;
    QToolButton* removeTrack_ = nullptr;
    int fileDropLane_ = -1;    // file trascinati sopra una traccia aggiunta: quale (-1 = sopra il video o A1)

    double fileDropX_ = -1.0;  // file trascinati sopra la linea del tempo: dove (-1 = nessuno)

    QPixmap cache_;
    bool cacheValid_ = false;
};

// I file locali (percorsi nativi) trascinati in `mime`; vuoto se non ce ne sono.
QStringList droppedFiles(const QMimeData* mime);

// Colore del blocco per un file (si ripetono dopo una decina di file).
QColor blockColor(int colorIndex);

}  // namespace editor::app
