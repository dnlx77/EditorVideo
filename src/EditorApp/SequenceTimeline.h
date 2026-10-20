#pragma once

#include "EditorCore/SegmentList.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace editor::core {
struct Waveform;
}

namespace editor::app {

// Linea del tempo del montaggio, su due corsie.
// Video: un blocco colorato per ogni pezzo della sequenza (un colore per file), largo quanto dura, con il nome del
// file. Clic = seleziona e sposta l'anteprima del montaggio lì; trascinamento = cambia l'ordine; doppio clic = riapre
// il pezzo nell'anteprima di partenza.
// Audio: la forma d'onda di ogni pezzo (o della traccia sostitutiva), i tratti senza audio barrati e il tratto
// selezionato. Trascinando si seleziona un tratto; la traccia sostitutiva si sposta trascinando la sua maniglia.
class SequenceTimeline : public QWidget
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

    void setMuted(std::vector<core::TimeRange> ranges);  // tempi della sequenza
    void setReplacement(std::optional<Replacement> replacement);
    // Tratto selezionato sulla corsia audio (tempi della sequenza); to <= from = nessuno.
    void setAudioSelection(double from, double to);
    bool hasAudioSelection() const { return selectionTo_ > selectionFrom_; }
    double audioSelectionFrom() const { return selectionFrom_; }
    double audioSelectionTo() const { return selectionTo_; }

    std::function<void(double)> onSeek;               // clic: tempo della sequenza
    std::function<void(int)> onSelect;                // pezzo selezionato
    std::function<void(int fragmentId, int toIndex)> onMove;  // trascinamento
    std::function<void(int)> onActivate;              // doppio clic
    std::function<void(double from, double to)> onAudioSelection;  // tratto selezionato (vuoto = tolto)
    std::function<void(double offset)> onReplacementMoved;         // traccia sostitutiva spostata
    // Miniatura del file `clipId` al tempo `sourceTime` (nei tempi del file); un'immagine vuota se non è ancora
    // pronta (chi la fornisce la prepara in background e poi chiede di ridisegnare).
    std::function<QImage(int clipId, double sourceTime)> thumbnailFor;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QRectF videoLane() const;
    QRectF audioLane() const;
    QRectF replacementHandle() const;  // maniglia della traccia sostitutiva (vuoto se non c'è)
    double total() const;
    double xForTime(double seconds) const;
    double timeForX(double x) const;
    int blockAt(double x) const;     // indice del blocco sotto x, -1 se nessuno
    int dropIndexAt(double x) const;  // dove finirebbe il blocco trascinato
    void paintAudio(QPainter& p);

    std::vector<Block> blocks_;
    std::vector<core::TimeRange> muted_;
    std::optional<Replacement> replacement_;
    double playhead_ = 0.0;
    int selected_ = 0;
    int dragIndex_ = -1;      // blocco che si sta trascinando
    double pressX_ = 0.0;
    bool dragging_ = false;
    double dragX_ = 0.0;

    double selectionFrom_ = 0.0;
    double selectionTo_ = 0.0;
    bool selecting_ = false;     // si sta trascinando una selezione sulla corsia audio
    double selectionAnchor_ = 0.0;
    bool movingReplacement_ = false;
    double replacementStart_ = 0.0;  // offset all'inizio del trascinamento
};

// Colore del blocco per un file (si ripetono dopo una decina di file).
QColor blockColor(int colorIndex);

}  // namespace editor::app
