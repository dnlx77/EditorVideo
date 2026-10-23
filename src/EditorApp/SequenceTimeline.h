#pragma once

#include "EditorApp/ZoomableTimeline.h"
#include "EditorCore/TimeUtil.h"

#include <QtCore/QStringList>
#include <QtGui/QImage>
#include <QtGui/QPixmap>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QMimeData;

namespace editor::core {
struct Waveform;
}

namespace editor::app {

// Linea del tempo del montaggio, su due corsie.
// Video: un blocco colorato per ogni pezzo della sequenza (un colore per file), largo quanto dura, con il nome del
// file. Clic = seleziona e sposta l'anteprima del montaggio lì; trascinamento = sposta il pezzo avanti o indietro
// (da solo: gli altri restano dove sono, e fra i pezzi restano spazi vuoti); doppio clic = riapre il pezzo
// nell'anteprima di partenza. Clic su uno spazio vuoto = lo seleziona (Canc lo chiude).
// Audio: la forma d'onda di ogni pezzo (o della traccia sostitutiva), i tratti senza audio barrati e il tratto
// selezionato. Trascinando si seleziona un tratto; la traccia sostitutiva si sposta trascinando la sua maniglia.
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
    };

    // La traccia audio sostitutiva, nella sequenza.
    struct Replacement
    {
        QString label;
        double offset = 0.0;  // dove inizia (negativo = l'inizio è prima della sequenza)
        double duration = 0.0;
        std::shared_ptr<const core::Waveform> waveform;  // nullptr = in calcolo
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

    void setMuted(std::vector<core::TimeRange> ranges);  // tempi della sequenza
    void setReplacement(std::optional<Replacement> replacement);
    // Quando finisce una forma d'onda: aggiorna solo quelle dei blocchi (`forClip` dà quella di un file) e della traccia
    // sostitutiva, senza rifare il resto.
    void setWaveforms(const std::function<std::shared_ptr<const core::Waveform>(int clipId)>& forClip,
                      const std::shared_ptr<const core::Waveform>& replacement);
    // Tratto selezionato sulla corsia audio (tempi della sequenza); to <= from = nessuno.
    void setAudioSelection(double from, double to);
    bool hasAudioSelection() const { return selectionTo_ > selectionFrom_; }
    double audioSelectionFrom() const { return selectionFrom_; }
    double audioSelectionTo() const { return selectionTo_; }

    std::function<void(double)> onSeek;               // clic: tempo della sequenza
    std::function<void(int)> onSelect;                // pezzo selezionato
    // Trascinamento di un pezzo: dove può andare se lo si vuole far iniziare a `desired` (vedi
    // Sequence::nearestFreeStart), e dove è stato lasciato.
    std::function<double(int fragmentId, double desired)> freeStart;
    std::function<void(int fragmentId, double start)> onMoveTo;
    std::function<void(int)> onActivate;              // doppio clic
    std::function<void(double from, double to)> onAudioSelection;  // tratto selezionato (vuoto = tolto)
    std::function<void(double offset)> onReplacementMoved;         // traccia sostitutiva spostata
    // File trascinati da Esplora file e lasciati qui: vanno nella sequenza a partire dalla posizione `index`.
    std::function<void(const QStringList& paths, int index)> onFilesDropped;
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
    QRectF audioLane() const;
    std::vector<QRectF> blockRects(const QRectF& lane) const;  // il riquadro di ogni blocco nella corsia
    QRectF replacementHandle() const;  // maniglia della traccia sostitutiva (vuoto se non c'è)
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
    void paintAudio(QPainter& p);

    std::vector<Block> blocks_;
    double total_ = 0.0;  // durata della sequenza: somma dei blocchi, calcolata in setBlocks
    std::vector<double> starts_;  // inizio di ogni blocco (dopo il suo spazio vuoto), calcolato in setBlocks
    std::vector<core::TimeRange> muted_;
    std::optional<Replacement> replacement_;
    double playhead_ = 0.0;
    int selected_ = 0;
    int selectedGap_ = 0;
    int dragIndex_ = -1;      // blocco che si sta trascinando
    double pressX_ = 0.0;
    bool dragging_ = false;
    double dragGrab_ = 0.0;   // distanza (in secondi) fra l'inizio del blocco e il punto in cui lo si è preso
    double dragStart_ = 0.0;  // dove andrà il blocco trascinato se lo si lascia adesso

    double selectionFrom_ = 0.0;
    double selectionTo_ = 0.0;
    bool selecting_ = false;     // si sta trascinando una selezione sulla corsia audio
    double selectionAnchor_ = 0.0;
    bool scrubbing_ = false;     // si sta trascinando il cursore sul righello
    bool movingReplacement_ = false;
    double replacementStart_ = 0.0;  // offset all'inizio del trascinamento

    double fileDropX_ = -1.0;  // file trascinati sopra la linea del tempo: dove (-1 = nessuno)

    QPixmap cache_;
    bool cacheValid_ = false;
};

// I file locali (percorsi nativi) trascinati in `mime`; vuoto se non ce ne sono.
QStringList droppedFiles(const QMimeData* mime);

// Colore del blocco per un file (si ripetono dopo una decina di file).
QColor blockColor(int colorIndex);

}  // namespace editor::app
