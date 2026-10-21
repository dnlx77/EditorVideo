#include "EditorApp/SequenceTimeline.h"

#include "EditorCore/MediaProbe.h"
#include "EditorCore/Waveform.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

constexpr double kTop = 16;          // spazio per il triangolo del cursore
constexpr double kVideoHeight = 54;
constexpr double kLaneGap = 4;
constexpr double kHandleHeight = 14;  // maniglia della traccia sostitutiva

// Forma d'onda di [sourceFrom, sourceTo) di un file, disegnata in `r` (una linea verticale per pixel), solo fra
// visibleLeft e visibleRight: con lo zoom `r` può essere molto più largo della parte che si vede.
void drawWave(QPainter& p, const QRectF& r, const core::Waveform& wave, double sourceFrom, double sourceTo,
              const QColor& color, double visibleLeft, double visibleRight)
{
    if (r.width() < 1 || sourceTo <= sourceFrom)
        return;
    p.setPen(QPen(color, 1));
    const double middle = r.center().y();
    const double half = r.height() / 2 - 1;
    const double perPixel = (sourceTo - sourceFrom) / r.width();
    const double first = std::ceil(std::max(r.left(), visibleLeft - 1));
    const double last = std::min(r.right(), visibleRight + 1);
    for (double x = first; x < last; x += 1) {
        const double t = sourceFrom + (x - r.left()) * perPixel;
        // Radice quadrata: i suoni deboli si vedono comunque.
        const double h = std::max(0.5, std::sqrt(wave.peak(t, t + perPixel)) * half);
        p.drawLine(QPointF(x, middle - h), QPointF(x, middle + h));
    }
}

}  // namespace

QColor blockColor(int colorIndex)
{
    static const QColor colors[] = {{66, 133, 214}, {214, 120, 60}, {82, 168, 98}, {170, 92, 190}, {196, 168, 54},
                                    {60, 170, 175}, {200, 80, 110}, {120, 120, 210}, {150, 150, 90}, {90, 150, 200}};
    return colors[static_cast<std::size_t>(std::abs(colorIndex)) % std::size(colors)];
}

SequenceTimeline::SequenceTimeline(QWidget* parent)
    : ZoomableTimeline(parent)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setToolTip(QStringLiteral("Video: clic per andare lì · trascina per spostare il pezzo · doppio clic per riaprirlo "
                              "a sinistra · Canc: togli · S: dividi al cursore\n"
                              "Audio: trascina per selezionare un tratto (o I e O nel Montaggio) · M: togli l'audio · "
                              "Maiusc+M: rimettilo · la traccia sostitutiva si sposta trascinando la sua maniglia\n"
                              "Ctrl+rotellina (o + e -): ingrandisci e riduci · rotellina: scorri"));
}

QSize SequenceTimeline::sizeHint() const
{
    return {800, 142 + kControlsHeight};
}

QSize SequenceTimeline::minimumSizeHint() const
{
    return {200, 122 + kControlsHeight};
}

void SequenceTimeline::setBlocks(std::vector<Block> blocks)
{
    blocks_ = std::move(blocks);
    total_ = 0.0;
    for (const Block& b : blocks_)
        total_ += b.length;
    if (std::none_of(blocks_.begin(), blocks_.end(), [this](const Block& b) { return b.fragmentId == selected_; }))
        selected_ = 0;
    setTimelineDuration(total_);
    invalidate();
}

void SequenceTimeline::setPlayhead(double seconds)
{
    if (std::abs(seconds - playhead_) < 1e-6)
        return;
    update(cursorRect(playhead_));
    playhead_ = seconds;
    update(cursorRect(playhead_));
    followPlayhead(seconds);
}

QRect SequenceTimeline::cursorRect(double seconds) const
{
    // Linea larga 2 e triangolino largo 10, con l'antialiasing: qualche pixel in più per lato.
    const double x = xForTime(seconds);
    return QRectF(x - 8, 0, 16, height()).toAlignedRect();
}

void SequenceTimeline::thumbnailsChanged()
{
    invalidate();
}

void SequenceTimeline::invalidate()
{
    cacheValid_ = false;
    update();
}

void SequenceTimeline::resizeEvent(QResizeEvent* event)
{
    cacheValid_ = false;
    ZoomableTimeline::resizeEvent(event);
}

void SequenceTimeline::changeEvent(QEvent* event)
{
    // Colori e carattere entrano nella parte disegnata una volta sola.
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange
        || event->type() == QEvent::EnabledChange || event->type() == QEvent::StyleChange)
        invalidate();
    QWidget::changeEvent(event);
}

void SequenceTimeline::setSelected(int fragmentId)
{
    selected_ = fragmentId;
    invalidate();
}

void SequenceTimeline::setMuted(std::vector<core::TimeRange> ranges)
{
    muted_ = std::move(ranges);
    invalidate();
}

void SequenceTimeline::setReplacement(std::optional<Replacement> replacement)
{
    if (!movingReplacement_)
        replacement_ = std::move(replacement);
    invalidate();
}

void SequenceTimeline::setAudioSelection(double from, double to)
{
    selectionFrom_ = from;
    selectionTo_ = std::max(from, to);
    invalidate();
}

QRectF SequenceTimeline::videoLane() const
{
    return QRectF(6, kTop, std::max(1, width() - 12), kVideoHeight);
}

QRectF SequenceTimeline::audioLane() const
{
    const double top = kTop + kVideoHeight + kLaneGap;
    return QRectF(6, top, std::max(1, width() - 12), std::max(20.0, height() - top - 6 - kControlsHeight));
}

QRectF SequenceTimeline::visiblePart(const QRectF& r) const
{
    return QRectF(QPointF(std::max(r.left(), visibleLeft() - 4), r.top()), QPointF(std::min(r.right(), visibleRight() + 4), r.bottom()));
}

std::vector<QRectF> SequenceTimeline::blockRects(const QRectF& lane) const
{
    std::vector<QRectF> rects;
    rects.reserve(blocks_.size());
    double start = 0.0;
    for (const Block& b : blocks_) {
        rects.emplace_back(QPointF(xForTime(start) + 1, lane.top() + 2), QPointF(xForTime(start + b.length) - 1, lane.bottom() - 2));
        start += b.length;
    }
    return rects;
}

QRectF SequenceTimeline::replacementHandle() const
{
    if (!replacement_ || total() <= 0)
        return {};
    const QRectF lane = audioLane();
    const double left = std::max(lane.left(), xForTime(std::max(0.0, replacement_->offset)));
    const double right = xForTime(std::min(total(), replacement_->offset + replacement_->duration));
    if (right <= left)
        return {};
    return QRectF(QPointF(left, lane.top()), QPointF(right, lane.top() + kHandleHeight));
}

int SequenceTimeline::blockAt(double x) const
{
    const double t = timeForX(x);
    double start = 0.0;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (t >= start && t < start + blocks_[i].length)
            return static_cast<int>(i);
        start += blocks_[i].length;
    }
    return blocks_.empty() ? -1 : static_cast<int>(blocks_.size()) - 1;
}

int SequenceTimeline::dropIndexAt(double x) const
{
    // Il blocco va prima del primo blocco la cui metà sta a destra del punto.
    const double t = timeForX(x);
    double start = 0.0;
    int index = 0;
    for (const Block& b : blocks_) {
        if (t < start + b.length / 2)
            return index;
        start += b.length;
        ++index;
    }
    return index;
}

void SequenceTimeline::paintEvent(QPaintEvent*)
{
    const qreal ratio = devicePixelRatioF();
    const QSize pixels = (QSizeF(size()) * ratio).toSize();
    if (!cacheValid_ || cache_.size() != pixels || cache_.devicePixelRatio() != ratio) {
        cache_ = QPixmap(pixels);
        cache_.setDevicePixelRatio(ratio);
        cache_.fill(Qt::transparent);
        QPainter cp(&cache_);
        // Come un QPainter sul widget: carattere (con i punti calcolati per lo schermo del widget) e penna.
        cp.setFont(QFont(font(), this));
        cp.setPen(palette().color(foregroundRole()));
        paintStatic(cp);
        cacheValid_ = true;
    }
    QPainter p(this);
    p.drawPixmap(0, 0, cache_);
    if (blocks_.empty())
        return;

    // Cursore, su tutte e due le corsie
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF a = videoLane();
    const QRectF audio = audioLane();
    const double x = xForTime(playhead_);
    if (x < a.left() - 1 || x > a.right() + 1)
        return;  // con lo zoom il cursore può stare fuori dalla parte visibile
    const QColor head = palette().color(QPalette::Text);
    p.setPen(QPen(head, 2));
    p.drawLine(QPointF(x, a.top()), QPointF(x, audio.bottom() + 3));
    QPainterPath triangle;
    triangle.moveTo(x - 5, a.top() - 7);
    triangle.lineTo(x + 5, a.top() - 7);
    triangle.lineTo(x, a.top() - 1);
    triangle.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(head);
    p.drawPath(triangle);
}

void SequenceTimeline::paintStatic(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF a = videoLane();
    const QRectF audio = audioLane();
    const QPalette pal = palette();
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(a, 4, 4);
    p.drawRoundedRect(audio, 4, 4);

    QFont small = p.font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);

    if (blocks_.empty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(a, Qt::AlignCenter,
                   QStringLiteral("Sequenza vuota: scegli un file, segna I e O nell'anteprima di sinistra e premi A"));
        p.drawText(audio, Qt::AlignCenter, QStringLiteral("Audio"));
        return;
    }

    // Blocchi: solo quelli che si vedono, e di ognuno solo la parte visibile.
    const std::vector<QRectF> rects = blockRects(a);
    p.save();
    p.setClipRect(a.adjusted(-1, -4, 1, 4));
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        const QRectF& full = rects[i];
        if (full.right() < visibleLeft() - 4 || full.left() > visibleRight() + 4)
            continue;
        const QRectF r = visiblePart(full);
        QColor color = blockColor(b.colorIndex);
        if (dragging_ && static_cast<int>(i) == dragIndex_)
            color.setAlpha(110);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(r, 3, 3);

        // Miniature: una all'inizio del pezzo e altre a distanza regolare se il blocco è largo.
        bool thumbnails = false;
        if (thumbnailFor && full.width() > 24) {
            // Le posizioni si contano su tutto il blocco (così non cambiano scorrendo), ma si chiedono solo
            // quelle visibili.
            const double slot = full.height() * 16.0 / 9.0;
            const int count = std::max(1, static_cast<int>(full.width() / slot));
            const int first = std::clamp(static_cast<int>(std::floor((r.left() - full.left()) / slot)), 0, count - 1);
            const int last = std::clamp(static_cast<int>(std::floor((r.right() - full.left()) / slot)), 0, count - 1);
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(r, 3, 3);
            p.setClipPath(clip, Qt::IntersectClip);
            for (int k = first; k <= last; ++k) {
                const QImage image = thumbnailFor(b.clipId, b.sourceStart + k * b.length / count);
                if (image.isNull())
                    continue;
                const QSizeF size = QSizeF(image.size()).scaled(QSizeF(slot, full.height()), Qt::KeepAspectRatioByExpanding);
                const QRectF target(QPointF(full.left() + k * slot, full.top()), size);
                p.drawImage(target, image);
                thumbnails = true;
            }
            if (dragging_ && static_cast<int>(i) == dragIndex_)
                p.fillRect(r, QColor(255, 255, 255, 140));
            p.restore();
        }
        p.setPen(b.fragmentId == selected_ ? QPen(pal.color(QPalette::Highlight), 3) : QPen(color.darker(140), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 3, 3);
        if (thumbnails && r.width() > 30) {
            // Sopra le miniature: nome e durata su una striscia scura, nel colore del file.
            const QRectF band(QPointF(r.left() + 1, r.bottom() - p.fontMetrics().height() - 3), QPointF(r.right() - 1, r.bottom() - 1));
            p.fillRect(band, QColor(0, 0, 0, 150));
            p.fillRect(QRectF(band.topLeft(), QSizeF(4, band.height())), color);
            p.setPen(Qt::white);
            const QString text = QStringLiteral("%1  %2").arg(
                b.label, QString::fromStdString(core::formatDuration(b.length)).mid(3));
            p.drawText(band.adjusted(7, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(band.width() - 10)));
        } else if (r.width() > 30) {
            p.setPen(Qt::white);
            const QString text = QStringLiteral("%1\n%2").arg(
                b.label, QString::fromStdString(core::formatDuration(b.length)).mid(3));  // mm:ss.mmm
            p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWrapAnywhere,
                       p.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(r.width() * 2)));
        }
    }

    p.restore();
    paintAudio(p);

    // Dove finirebbe il blocco trascinato
    if (dragging_) {
        const int drop = dropIndexAt(dragX_);
        double at = 0.0;
        for (int i = 0; i < drop && i < static_cast<int>(blocks_.size()); ++i)
            at += blocks_[i].length;
        const double x = std::clamp(xForTime(at), a.left(), a.right());
        p.setPen(QPen(pal.color(QPalette::Highlight), 3));
        p.drawLine(QPointF(x, a.top() - 4), QPointF(x, a.bottom() + 4));
    }
}

void SequenceTimeline::paintAudio(QPainter& p)
{
    const QRectF lane = audioLane();
    const QPalette pal = palette();
    p.save();
    p.setClipRect(lane);

    if (!replacement_) {
        // Audio originale: la forma d'onda di ogni pezzo, nel colore del suo file.
        const std::vector<QRectF> rects = blockRects(lane);
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            const Block& b = blocks_[i];
            const QRectF& full = rects[i];
            if (full.right() < visibleLeft() - 4 || full.left() > visibleRight() + 4)
                continue;
            const QRectF r = visiblePart(full);
            const QColor color = blockColor(b.colorIndex);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(color.red(), color.green(), color.blue(), 45));
            p.drawRoundedRect(r, 3, 3);
            if (!b.waveform) {
                p.setPen(pal.color(QPalette::PlaceholderText));
                if (r.width() > 60)
                    p.drawText(r, Qt::AlignCenter, QStringLiteral("forma d'onda..."));
            } else if (b.waveform->empty()) {
                p.setPen(pal.color(QPalette::PlaceholderText));
                if (r.width() > 60)
                    p.drawText(r, Qt::AlignCenter, QStringLiteral("senza audio"));
            } else {
                drawWave(p, full, *b.waveform, b.sourceStart, b.sourceStart + b.length, color.darker(110), visibleLeft(),
                         visibleRight());
            }
        }
    } else {
        // Traccia sostitutiva: un solo blocco, con la maniglia per spostarlo; l'audio dei file è escluso.
        p.setPen(pal.color(QPalette::PlaceholderText));
        const QRectF handle = visiblePart(replacementHandle());
        const double left = xForTime(std::max(0.0, replacement_->offset));
        const double right = xForTime(std::min(total(), replacement_->offset + replacement_->duration));
        if (left > lane.left() + 40)
            p.drawText(QRectF(QPointF(lane.left(), lane.top()), QPointF(std::min(left, lane.right()), lane.bottom())),
                       Qt::AlignCenter, QStringLiteral("silenzio"));
        if (right > left && right > visibleLeft() && left < visibleRight()) {
            const QColor color(214, 160, 40);
            const QRectF body(QPointF(left + 1, lane.top() + kHandleHeight), QPointF(right - 1, lane.bottom() - 2));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(color.red(), color.green(), color.blue(), 50));
            p.drawRect(visiblePart(body));
            p.setBrush(color);
            p.drawRoundedRect(handle.adjusted(1, 1, -1, 0), 3, 3);
            p.setPen(Qt::black);
            p.drawText(handle.adjusted(5, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(QStringLiteral("♪ %1  (trascina qui per spostarla)").arg(replacement_->label),
                                                  Qt::ElideRight, static_cast<int>(handle.width() - 10)));
            if (replacement_->waveform && !replacement_->waveform->empty()) {
                // La parte della traccia che si vede: dal tempo 0 della sequenza (o dall'inizio della traccia).
                const double from = std::max(0.0, -replacement_->offset);
                const double to = std::min(replacement_->duration, total() - replacement_->offset);
                drawWave(p, body, *replacement_->waveform, from, to, color.darker(130), visibleLeft(), visibleRight());
            } else if (!replacement_->waveform) {
                p.setPen(pal.color(QPalette::PlaceholderText));
                p.drawText(visiblePart(body), Qt::AlignCenter, QStringLiteral("forma d'onda..."));
            }
        }
    }

    // Tratti senza audio: barrati.
    for (const core::TimeRange& m : muted_) {
        const QRectF r = visiblePart(QRectF(QPointF(xForTime(m.start), lane.top() + 1), QPointF(xForTime(m.end), lane.bottom() - 1)));
        if (r.width() < 1)
            continue;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 40, 40, 140));
        p.drawRect(r);
        p.setBrush(QBrush(QColor(230, 70, 60), Qt::BDiagPattern));
        p.drawRect(r);
        p.setPen(QPen(QColor(230, 70, 60), 2));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        if (r.width() > 50) {
            p.setPen(Qt::white);
            p.drawText(r, Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("senza audio"));
        }
    }

    // Tratto selezionato
    if (hasAudioSelection()) {
        const QRectF r = visiblePart(QRectF(QPointF(xForTime(selectionFrom_), lane.top()), QPointF(xForTime(selectionTo_), lane.bottom())));
        QColor highlight = pal.color(QPalette::Highlight);
        highlight.setAlpha(70);
        p.setPen(QPen(pal.color(QPalette::Highlight), 2));
        p.setBrush(highlight);
        p.drawRect(r);
    }
    p.restore();
}

void SequenceTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || blocks_.empty())
        return;
    pressX_ = event->position().x();
    const double t = timeForX(pressX_);

    if (event->position().y() >= audioLane().top() - kLaneGap / 2) {
        // Corsia audio: maniglia della traccia sostitutiva, oppure inizio di una selezione.
        if (visiblePart(replacementHandle()).contains(event->position())) {
            movingReplacement_ = true;
            replacementStart_ = replacement_->offset;
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        selecting_ = true;
        selectionAnchor_ = t;
        setAudioSelection(t, t);
        playhead_ = t;
        if (onSeek)
            onSeek(t);
        return;
    }

    dragIndex_ = blockAt(pressX_);
    dragging_ = false;
    if (dragIndex_ >= 0) {
        selected_ = blocks_[dragIndex_].fragmentId;
        if (onSelect)
            onSelect(selected_);
    }
    playhead_ = t;
    invalidate();
    if (onSeek)
        onSeek(playhead_);
}

void SequenceTimeline::mouseMoveEvent(QMouseEvent* event)
{
    const double x = event->position().x();
    if (!(event->buttons() & Qt::LeftButton)) {
        // Sulla maniglia della traccia sostitutiva il cursore diventa una mano.
        setCursor(visiblePart(replacementHandle()).contains(event->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    if (movingReplacement_) {
        replacement_->offset = replacementStart_ + (x - pressX_) / zoom().pixelsPerSecond();
        invalidate();
        return;
    }
    if (selecting_) {
        const double t = timeForX(x);
        setAudioSelection(std::min(selectionAnchor_, t), std::max(selectionAnchor_, t));
        return;
    }
    if (dragIndex_ < 0)
        return;
    dragX_ = x;
    if (!dragging_ && std::abs(dragX_ - pressX_) > 8)
        dragging_ = true;
    if (dragging_)
        invalidate();
}

void SequenceTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (movingReplacement_) {
        movingReplacement_ = false;
        setCursor(Qt::OpenHandCursor);
        if (onReplacementMoved && replacement_)
            onReplacementMoved(replacement_->offset);
        return;
    }
    if (selecting_) {
        selecting_ = false;
        // Un clic senza trascinare toglie la selezione.
        if (std::abs(event->position().x() - pressX_) < 4)
            setAudioSelection(0.0, 0.0);
        if (onAudioSelection)
            onAudioSelection(selectionFrom_, selectionTo_);
        return;
    }
    if (dragging_ && dragIndex_ >= 0) {
        int to = dropIndexAt(event->position().x());
        if (to > dragIndex_)
            --to;  // togliendo il blocco dalla sua posizione, quelli dopo scalano di uno
        const int fragment = blocks_[dragIndex_].fragmentId;
        dragging_ = false;
        dragIndex_ = -1;
        invalidate();
        if (onMove)
            onMove(fragment, std::min(to, static_cast<int>(blocks_.size()) - 1));
        return;
    }
    dragging_ = false;
    dragIndex_ = -1;
}

void SequenceTimeline::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->position().y() >= audioLane().top() - kLaneGap / 2)
        return;
    const int index = blockAt(event->position().x());
    if (index >= 0 && onActivate)
        onActivate(blocks_[index].fragmentId);
}

}  // namespace editor::app
