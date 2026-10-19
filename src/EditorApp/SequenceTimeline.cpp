#include "EditorApp/SequenceTimeline.h"

#include "EditorCore/MediaProbe.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>

namespace editor::app {

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
    setToolTip(QStringLiteral("Clic: vai lì · trascina: sposta il pezzo · doppio clic: riaprilo a sinistra · "
                              "Canc: togli · S: dividi al cursore"));
}

QSize SequenceTimeline::sizeHint() const
{
    return {800, 72};
}

QSize SequenceTimeline::minimumSizeHint() const
{
    return {200, 60};
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

QRectF SequenceTimeline::area() const
{
    return QRectF(6, 16, std::max(1, width() - 12), height() - 22);
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
    const QRectF a = area();
    const double t = total();
    return t <= 0 ? a.left() : a.left() + std::clamp(seconds / t, 0.0, 1.0) * a.width();
}

double SequenceTimeline::timeForX(double x) const
{
    const QRectF a = area();
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
    const QRectF a = area();
    const QPalette pal = palette();
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(a, 4, 4);

    QFont small = p.font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);

    if (blocks_.empty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(a, Qt::AlignCenter,
                   QStringLiteral("Sequenza vuota: scegli un file, segna I e O nell'anteprima di sinistra e premi A"));
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
        p.setPen(b.fragmentId == selected_ ? QPen(pal.color(QPalette::Highlight), 3) : QPen(color.darker(140), 1));
        p.setBrush(color);
        p.drawRoundedRect(r, 3, 3);
        if (r.width() > 30) {
            p.setPen(Qt::white);
            const QString text = QStringLiteral("%1\n%2").arg(
                b.label, QString::fromStdString(core::formatDuration(b.length)).mid(3));  // mm:ss.mmm
            p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWrapAnywhere,
                       p.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(r.width() * 2)));
        }
    }

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

    // Cursore
    const double x = xForTime(playhead_);
    const QColor head = pal.color(QPalette::Text);
    p.setPen(QPen(head, 2));
    p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom() + 3));
    QPainterPath triangle;
    triangle.moveTo(x - 5, a.top() - 7);
    triangle.lineTo(x + 5, a.top() - 7);
    triangle.lineTo(x, a.top() - 1);
    triangle.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(head);
    p.drawPath(triangle);
}

void SequenceTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || blocks_.empty())
        return;
    pressX_ = event->position().x();
    dragIndex_ = blockAt(pressX_);
    dragging_ = false;
    if (dragIndex_ >= 0) {
        selected_ = blocks_[dragIndex_].fragmentId;
        if (onSelect)
            onSelect(selected_);
    }
    playhead_ = timeForX(pressX_);
    update();
    if (onSeek)
        onSeek(playhead_);
}

void SequenceTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (dragIndex_ < 0 || !(event->buttons() & Qt::LeftButton))
        return;
    dragX_ = event->position().x();
    if (!dragging_ && std::abs(dragX_ - pressX_) > 8)
        dragging_ = true;
    if (dragging_)
        update();
}

void SequenceTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
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
    const int index = blockAt(event->position().x());
    if (index >= 0 && onActivate)
        onActivate(blocks_[index].fragmentId);
}

}  // namespace editor::app
