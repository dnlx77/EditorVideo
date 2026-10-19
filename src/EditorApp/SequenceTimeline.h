#pragma once

#include <QtWidgets/QWidget>

#include <functional>
#include <vector>

namespace editor::app {

// Linea del tempo del montaggio: un blocco colorato per ogni pezzo della sequenza (un colore per file), largo quanto
// dura, con il nome del file. Clic = seleziona e sposta l'anteprima del montaggio lì; trascinamento = cambia l'ordine;
// doppio clic = riapre il pezzo nell'anteprima di partenza.
class SequenceTimeline : public QWidget
{
public:
    struct Block
    {
        int fragmentId = 0;
        int colorIndex = 0;  // di solito la posizione del file nel progetto
        QString label;       // es. "vacanze.mp4"
        double length = 0.0;
    };

    explicit SequenceTimeline(QWidget* parent = nullptr);

    void setBlocks(std::vector<Block> blocks);
    void setPlayhead(double seconds);
    void setSelected(int fragmentId);
    int selected() const { return selected_; }

    std::function<void(double)> onSeek;               // clic: tempo della sequenza
    std::function<void(int)> onSelect;                // pezzo selezionato
    std::function<void(int fragmentId, int toIndex)> onMove;  // trascinamento
    std::function<void(int)> onActivate;              // doppio clic

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QRectF area() const;
    double total() const;
    double xForTime(double seconds) const;
    double timeForX(double x) const;
    int blockAt(double x) const;     // indice del blocco sotto x, -1 se nessuno
    int dropIndexAt(double x) const;  // dove finirebbe il blocco trascinato

    std::vector<Block> blocks_;
    double playhead_ = 0.0;
    int selected_ = 0;
    int dragIndex_ = -1;      // blocco che si sta trascinando
    double pressX_ = 0.0;
    bool dragging_ = false;
    double dragX_ = 0.0;
};

// Colore del blocco per un file (si ripetono dopo una decina di file).
QColor blockColor(int colorIndex);

}  // namespace editor::app
