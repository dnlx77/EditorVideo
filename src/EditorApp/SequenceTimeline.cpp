#include "EditorApp/SequenceTimeline.h"

#include "EditorCore/MediaProbe.h"
#include "EditorCore/Waveform.h"

#include <QtGui/QMouseEvent>
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

// Forma d'onda di [sourceFrom, sourceTo) di un file, disegnata in `r` (una linea verticale per pixel).
void drawWave(QPainter& p, const QRectF& r, const core::Waveform& wave, double sourceFrom, double sourceTo,
              const QColor& color)
{
    if (r.width() < 1 || sourceTo <= sourceFrom)
        return;
    p.setPen(QPen(color, 1));
    const double middle = r.center().y();
    const double half = r.height() / 2 - 1;
    const double perPixel = (sourceTo - sourceFrom) / r.width();
    for (double x = std::ceil(r.left()); x < r.right(); x += 1) {
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
    : QWidget(parent)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setToolTip(QStringLiteral("Video: clic per andare lì · trascina per spostare il pezzo · doppio clic per riaprirlo "
                              "a sinistra · Canc: togli · S: dividi al cursore\n"
                              "Audio: trascina per selezionare un tratto (o I e O nel Montaggio) · M: togli l'audio · "
                              "Maiusc+M: rimettilo · la traccia sostitutiva si sposta trascinando la sua maniglia"));
}

QSize SequenceTimeline::sizeHint() const
{
    return {800, 142};
}

QSize SequenceTimeline::minimumSizeHint() const
{
    return {200, 122};
}

void SequenceTimeline::setBlocks(std::vector<Block> blocks)
{
    blocks_ = std::move(blocks);
    if (std::none_of(blocks_.begin(), blocks_.end(), [this](const Block& b) { return b.fragmentId == selected_; }))
        selected_ = 0;
    update();
}

void SequenceTimeline::setPlayhead(double seconds)
{
    if (std::abs(seconds - playhead_) < 1e-6)
        return;
    playhead_ = seconds;
    update();
}

void SequenceTimeline::setSelected(int fragmentId)
{
    selected_ = fragmentId;
    update();
}

void SequenceTimeline::setMuted(std::vector<core::TimeRange> ranges)
{
    muted_ = std::move(ranges);
    update();
}

void SequenceTimeline::setReplacement(std::optional<Replacement> replacement)
{
    if (!movingReplacement_)
        replacement_ = std::move(replacement);
    update();
}

void SequenceTimeline::setAudioSelection(double from, double to)
{
    selectionFrom_ = from;
    selectionTo_ = std::max(from, to);
    update();
}

QRectF SequenceTimeline::videoLane() const
{
    return QRectF(6, kTop, std::max(1, width() - 12), kVideoHeight);
}

QRectF SequenceTimeline::audioLane() const
{
    const double top = kTop + kVideoHeight + kLaneGap;
    return QRectF(6, top, std::max(1, width() - 12), std::max(20.0, height() - top - 6));
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

double SequenceTimeline::total() const
{
    double sum = 0.0;
    for (const Block& b : blocks_)
        sum += b.length;
    return sum;
}

double SequenceTimeline::xForTime(double seconds) const
{
    const QRectF a = videoLane();
    const double t = total();
    return t <= 0 ? a.left() : a.left() + std::clamp(seconds / t, 0.0, 1.0) * a.width();
}

double SequenceTimeline::timeForX(double x) const
{
    const QRectF a = videoLane();
    return std::clamp((x - a.left()) / a.width(), 0.0, 1.0) * total();
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
    QPainter p(this);
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

    // Blocchi
    double start = 0.0;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        const QRectF r(QPointF(xForTime(start) + 1, a.top() + 2), QPointF(xForTime(start + b.length) - 1, a.bottom() - 2));
        start += b.length;
        QColor color = blockColor(b.colorIndex);
        if (dragging_ && static_cast<int>(i) == dragIndex_)
            color.setAlpha(110);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(r, 3, 3);

        // Miniature: una all'inizio del pezzo e altre a distanza regolare se il blocco è largo.
        bool thumbnails = false;
        if (thumbnailFor && r.width() > 24) {
            const double slot = r.height() * 16.0 / 9.0;
            const int count = std::max(1, static_cast<int>(r.width() / slot));
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(r, 3, 3);
            p.setClipPath(clip);
            for (int k = 0; k < count; ++k) {
                const QImage image = thumbnailFor(b.clipId, b.sourceStart + k * b.length / count);
                if (image.isNull())
                    continue;
                const QSizeF size = QSizeF(image.size()).scaled(QSizeF(slot, r.height()), Qt::KeepAspectRatioByExpanding);
                const QRectF target(QPointF(r.left() + k * slot, r.top()), size);
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

    paintAudio(p);

    // Dove finirebbe il blocco trascinato
    if (dragging_) {
        const int drop = dropIndexAt(dragX_);
        double at = 0.0;
        for (int i = 0; i < drop && i < static_cast<int>(blocks_.size()); ++i)
            at += blocks_[i].length;
        const double x = xForTime(at);
        p.setPen(QPen(pal.color(QPalette::Highlight), 3));
        p.drawLine(QPointF(x, a.top() - 4), QPointF(x, a.bottom() + 4));
    }

    // Cursore, su tutte e due le corsie
    const double x = xForTime(playhead_);
    const QColor head = pal.color(QPalette::Text);
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

void SequenceTimeline::paintAudio(QPainter& p)
{
    const QRectF lane = audioLane();
    const QPalette pal = palette();
    p.save();
    p.setClipRect(lane);

    if (!replacement_) {
        // Audio originale: la forma d'onda di ogni pezzo, nel colore del suo file.
        double start = 0.0;
        for (const Block& b : blocks_) {
            const QRectF r(QPointF(xForTime(start) + 1, lane.top() + 2), QPointF(xForTime(start + b.length) - 1, lane.bottom() - 2));
            start += b.length;
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
                drawWave(p, r, *b.waveform, b.sourceStart, b.sourceStart + b.length, color.darker(110));
            }
        }
    } else {
        // Traccia sostitutiva: un solo blocco, con la maniglia per spostarlo; l'audio dei file è escluso.
        p.setPen(pal.color(QPalette::PlaceholderText));
        const QRectF handle = replacementHandle();
        const double left = xForTime(std::max(0.0, replacement_->offset));
        const double right = xForTime(std::min(total(), replacement_->offset + replacement_->duration));
        if (left > lane.left() + 40)
            p.drawText(QRectF(QPointF(lane.left(), lane.top()), QPointF(left, lane.bottom())), Qt::AlignCenter,
                       QStringLiteral("silenzio"));
        if (right > left) {
            const QColor color(214, 160, 40);
            const QRectF body(QPointF(left + 1, lane.top() + kHandleHeight), QPointF(right - 1, lane.bottom() - 2));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(color.red(), color.green(), color.blue(), 50));
            p.drawRect(body);
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
                drawWave(p, body, *replacement_->waveform, from, to, color.darker(130));
            } else if (!replacement_->waveform) {
                p.setPen(pal.color(QPalette::PlaceholderText));
                p.drawText(body, Qt::AlignCenter, QStringLiteral("forma d'onda..."));
            }
        }
    }

    // Tratti senza audio: barrati.
    for (const core::TimeRange& m : muted_) {
        const QRectF r(QPointF(xForTime(m.start), lane.top() + 1), QPointF(xForTime(m.end), lane.bottom() - 1));
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
        const QRectF r(QPointF(xForTime(selectionFrom_), lane.top()), QPointF(xForTime(selectionTo_), lane.bottom()));
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
        if (replacementHandle().contains(event->position())) {
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
    update();
    if (onSeek)
        onSeek(playhead_);
}

void SequenceTimeline::mouseMoveEvent(QMouseEvent* event)
{
    const double x = event->position().x();
    if (!(event->buttons() & Qt::LeftButton)) {
        // Sulla maniglia della traccia sostitutiva il cursore diventa una mano.
        setCursor(replacementHandle().contains(event->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    if (movingReplacement_) {
        const double perPixel = total() / std::max(1.0, videoLane().width());
        replacement_->offset = replacementStart_ + (x - pressX_) * perPixel;
        update();
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
        update();
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
        update();
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
