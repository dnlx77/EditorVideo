#include "EditorApp/TimelineWidget.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

const QColor kInColor(70, 190, 90);
const QColor kOutColor(225, 80, 70);
const QColor kSegmentColor(70, 140, 230, 90);

}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Clic o trascinamento per spostarsi. Le tacche sono i fotogrammi chiave."));
}

QSize TimelineWidget::sizeHint() const
{
    return {400, 46};
}

QSize TimelineWidget::minimumSizeHint() const
{
    return {120, 46};
}

void TimelineWidget::setDuration(double seconds)
{
    duration_ = std::max(0.0, seconds);
    update();
}

void TimelineWidget::setKeyframes(std::vector<double> keyframes)
{
    keyframes_ = std::move(keyframes);
    update();
}

void TimelineWidget::setSegment(double in, double out, bool hasIn, bool hasOut)
{
    in_ = in;
    out_ = out;
    hasIn_ = hasIn;
    hasOut_ = hasOut;
    update();
}

void TimelineWidget::setPlayhead(double seconds)
{
    if (dragging_ || std::abs(seconds - playhead_) < 1e-6)
        return;
    playhead_ = seconds;
    update();
}

QRectF TimelineWidget::trackRect() const
{
    // Margini laterali per non tagliare i marcatori agli estremi; spazio in alto per le etichette IN/OUT.
    return QRectF(8, 14, std::max(1, width() - 16), height() - 20);
}

double TimelineWidget::xForTime(double seconds) const
{
    const QRectF track = trackRect();
    if (duration_ <= 0)
        return track.left();
    return track.left() + std::clamp(seconds / duration_, 0.0, 1.0) * track.width();
}

double TimelineWidget::timeForX(double x) const
{
    const QRectF track = trackRect();
    if (duration_ <= 0 || track.width() <= 0)
        return 0.0;
    return std::clamp((x - track.left()) / track.width(), 0.0, 1.0) * duration_;
}

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track = trackRect();
    const QPalette pal = palette();
    const bool enabled = isEnabled() && duration_ > 0;

    // Traccia
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(track, 3, 3);
    if (!enabled)
        return;

    // Segmento IN-OUT
    const double inX = xForTime(in_);
    const double outX = xForTime(out_);
    p.setBrush(kSegmentColor);
    p.drawRect(QRectF(QPointF(inX, track.top()), QPointF(outX, track.bottom())));

    // Fotogrammi chiave: una tacca per pixel al massimo (con migliaia di fotogrammi chiave basta così).
    QColor tick = pal.color(QPalette::Text);
    tick.setAlpha(110);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(tick, 1));
    int lastX = -1;
    for (const double t : keyframes_) {
        const int x = static_cast<int>(std::lround(xForTime(t)));
        if (x == lastX)
            continue;
        lastX = x;
        p.drawLine(QPointF(x, track.top() + 2), QPointF(x, track.top() + track.height() * 0.45));
    }
    p.setRenderHint(QPainter::Antialiasing);

    // Marcatori IN e OUT, con l'etichetta sopra la traccia
    QFont font = p.font();
    font.setPointSizeF(font.pointSizeF() * 0.8);
    font.setBold(true);
    p.setFont(font);
    const auto marker = [&](double x, const QColor& color, const QString& label, bool labelOnLeft) {
        p.setPen(QPen(color, 2));
        p.drawLine(QPointF(x, track.top() - 2), QPointF(x, track.bottom()));
        const QRectF textRect = labelOnLeft ? QRectF(x - 40, 0, 38, 13) : QRectF(x + 2, 0, 38, 13);
        p.drawText(textRect, (labelOnLeft ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, label);
    };
    if (hasIn_)
        marker(inX, kInColor, QStringLiteral("IN"), false);
    if (hasOut_)
        marker(outX, kOutColor, QStringLiteral("OUT"), true);

    // Cursore di riproduzione: linea con un triangolino in alto
    const double x = xForTime(playhead_);
    const QColor head = pal.color(QPalette::Text);
    p.setPen(QPen(head, 2));
    p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom() + 3));
    QPainterPath triangle;
    triangle.moveTo(x - 5, track.top() - 6);
    triangle.lineTo(x + 5, track.top() - 6);
    triangle.lineTo(x, track.top());
    triangle.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(head);
    p.drawPath(triangle);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || duration_ <= 0 || !isEnabled())
        return;
    dragging_ = true;
    playhead_ = timeForX(event->position().x());
    update();
    if (onScrub)
        onScrub(playhead_);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!dragging_)
        return;
    playhead_ = timeForX(event->position().x());
    update();
    if (onScrub)
        onScrub(playhead_);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (!dragging_ || event->button() != Qt::LeftButton)
        return;
    dragging_ = false;
    playhead_ = timeForX(event->position().x());
    update();
    if (onSeek)
        onSeek(playhead_);
}

}  // namespace editor::app
