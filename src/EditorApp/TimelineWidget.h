#pragma once

#include "EditorCore/SegmentList.h"

#include <QtGui/QPixmap>
#include <QtWidgets/QWidget>

#include <functional>
#include <vector>

namespace editor::app {

// Linea del tempo di un file: fotogrammi chiave (tacche), segmento IN-OUT evidenziato e cursore di riproduzione.
// Un clic o un trascinamento sposta il cursore.
class TimelineWidget : public QWidget
{
public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setDuration(double seconds);
    void setKeyframes(std::vector<double> keyframes);
    // Segmento evidenziato; hasIn/hasOut dicono se disegnare i marcatori IN e OUT.
    void setSegment(double in, double out, bool hasIn, bool hasOut);
    // Tratti tenuti (bande verdi); il resto della linea del tempo viene scurito. Vuoto = niente bande.
    void setKeptRanges(std::vector<core::TimeRange> ranges);
    void setPlayhead(double seconds);  // ignorato mentre l'utente sta trascinando

    std::function<void(double)> onScrub;  // durante il trascinamento (solo per aggiornare le etichette)
    std::function<void(double)> onSeek;   // al rilascio del mouse: spostarsi davvero lì

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // Tutto tranne il cursore (traccia, bande, tacche, IN e OUT) si disegna in cache_ e si rifà solo quando cambia;
    // a ogni ridisegno si copia cache_ e sopra si disegna il cursore.
    void invalidate();
    void paintStatic(QPainter& p);
    void paintCursor(QPainter& p);
    void movePlayhead(double seconds);           // ridisegna solo dove era e dove va il cursore
    QRect cursorRect(double seconds) const;
    QRectF trackRect() const;
    double xForTime(double seconds) const;
    double timeForX(double x) const;

    double duration_ = 0.0;
    std::vector<double> keyframes_;
    double in_ = 0.0;
    double out_ = 0.0;
    bool hasIn_ = false;
    bool hasOut_ = false;
    std::vector<core::TimeRange> kept_;
    double playhead_ = 0.0;
    bool dragging_ = false;
    QPixmap cache_;
    bool cacheValid_ = false;
};

}  // namespace editor::app
