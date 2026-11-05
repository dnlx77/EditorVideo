#include "AnimMiniTimeline.h"

#include "AnimColors.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QMenu>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {
constexpr double kSame = 1e-6;
constexpr double kSnapPixels = 5.0;
}  // namespace

AnimMiniTimeline::AnimMiniTimeline(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("miniLineaDelTempo"));
    setMouseTracking(false);
    setVisible(false);
    setToolTip(QStringLiteral("Le chiavi dell'animazione del pezzo: clic per portare il cursore, trascina un rombo per spostarlo, "
                              "clic destro per cambiarne il passaggio o toglierlo"));
}

std::vector<core::AnimGroup> AnimMiniTimeline::rows() const
{
    std::vector<core::AnimGroup> result;
    for (int g = 0; g < core::kAnimGroupCount; ++g) {
        const auto group = static_cast<core::AnimGroup>(g);
        if (std::any_of(marks_.begin(), marks_.end(), [&](const Mark& m) { return m.group == group; }))
            result.push_back(group);
    }
    return result;
}

void AnimMiniTimeline::setData(double from, double to, double cursor, std::vector<Mark> marks)
{
    const std::size_t before = rows().size();
    from_ = from;
    to_ = std::max(to, from + 1e-6);
    cursor_ = cursor;
    marks_ = std::move(marks);
    if (dragIndex_ >= 0 && dragIndex_ >= static_cast<int>(marks_.size()))
        dragIndex_ = -1;
    const bool show = !marks_.empty();
    if (show != isVisibleTo(parentWidget()) || rows().size() != before)
        updateGeometry();
    setVisible(show);
    update();
}

QSize AnimMiniTimeline::sizeHint() const
{
    return QSize(200, static_cast<int>(rows().size()) * kRowHeight + 6);
}

QSize AnimMiniTimeline::minimumSizeHint() const
{
    return QSize(120, static_cast<int>(rows().size()) * kRowHeight + 6);
}

double AnimMiniTimeline::x(double time) const
{
    const double left = kLabelWidth + 6.0;
    const double right = width() - 8.0;
    return left + (time - from_) / (to_ - from_) * std::max(1.0, right - left);
}

double AnimMiniTimeline::timeAt(double px) const
{
    const double left = kLabelWidth + 6.0;
    const double right = width() - 8.0;
    return std::clamp(from_ + (px - left) / std::max(1.0, right - left) * (to_ - from_), from_, to_);
}

double AnimMiniTimeline::y(core::AnimGroup group) const
{
    const std::vector<core::AnimGroup> r = rows();
    const auto it = std::find(r.begin(), r.end(), group);
    const int index = it == r.end() ? 0 : static_cast<int>(it - r.begin());
    return 3.0 + index * kRowHeight + kRowHeight / 2.0;
}

int AnimMiniTimeline::markAt(const QPointF& point) const
{
    int best = -1;
    double bestDistance = 7.0;
    for (int i = 0; i < static_cast<int>(marks_.size()); ++i) {
        const Mark& m = marks_[static_cast<std::size_t>(i)];
        if (m.time < from_ - kSame || m.time > to_ + kSame)
            continue;
        const double dx = std::abs(x(m.time) - point.x());
        const double dy = std::abs(y(m.group) - point.y());
        if (dy <= kRowHeight / 2.0 && dx <= bestDistance) {
            bestDistance = dx;
            best = i;
        }
    }
    return best;
}

QPointF AnimMiniTimeline::markCenterForTest(int index) const
{
    const Mark& m = marks_.at(static_cast<std::size_t>(index));
    return QPointF(x(m.time), y(m.group));
}

QPointF AnimMiniTimeline::pointForTimeForTest(core::AnimGroup group, double time) const
{
    return QPointF(x(time), y(group));
}

void AnimMiniTimeline::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPalette pal = palette();
    const double left = x(from_);
    const double right = x(to_);
    QFont small = font();
    small.setPointSizeF(std::max(6.5, small.pointSizeF() - 1.0));
    p.setFont(small);
    for (const core::AnimGroup group : rows()) {
        const QColor color = animGroupColor(group);
        const double cy = y(group);
        QColor band = color;
        band.setAlpha(34);
        p.setPen(Qt::NoPen);
        p.setBrush(band);
        p.drawRect(QRectF(left, cy - kRowHeight / 2.0, right - left, kRowHeight));
        p.setPen(pal.color(QPalette::WindowText));
        p.drawText(QRectF(0, cy - kRowHeight / 2.0, kLabelWidth, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(animGroupName(group)));
        QPolygonF line;
        for (int i = 0; i < static_cast<int>(marks_.size()); ++i) {
            const Mark& m = marks_[static_cast<std::size_t>(i)];
            if (m.group == group && m.time >= from_ - kSame && m.time <= to_ + kSame)
                line << QPointF(x(dragIndex_ == i && dragMoved_ ? dragValue_ : m.time), cy);
        }
        if (line.size() >= 2) {
            QColor stroke = color;
            stroke.setAlpha(190);
            p.setPen(QPen(stroke, 1.5));
            p.drawPolyline(line);
        }
    }
    const double half = 4.5;
    for (int i = 0; i < static_cast<int>(marks_.size()); ++i) {
        const Mark& m = marks_[static_cast<std::size_t>(i)];
        if (m.time < from_ - kSame || m.time > to_ + kSame)
            continue;
        const bool dragged = dragIndex_ == i && dragMoved_;
        const double mx = x(dragged ? dragValue_ : m.time);
        const double my = y(m.group);
        p.setBrush(animGroupColor(m.group));
        p.setPen(QPen(QColor(15, 15, 15), 1.1));
        switch (m.interp) {
            case core::Interp::Hold: p.drawRect(QRectF(mx - half * 0.8, my - half * 0.8, half * 1.6, half * 1.6)); break;
            case core::Interp::Smooth: p.drawEllipse(QPointF(mx, my), half * 0.9, half * 0.9); break;
            case core::Interp::Linear:
            case core::Interp::Bezier: {
                QPolygonF diamond;
                diamond << QPointF(mx, my - half) << QPointF(mx + half, my) << QPointF(mx, my + half) << QPointF(mx - half, my);
                p.drawPolygon(diamond);
                if (m.interp == core::Interp::Bezier) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(20, 20, 20));
                    p.drawEllipse(QPointF(mx, my), half * 0.28, half * 0.28);
                }
                break;
            }
        }
    }
    if (cursor_ >= from_ - kSame && cursor_ <= to_ + kSame) {
        const double cx = x(cursor_);
        p.setPen(QPen(QColor(235, 70, 70), 1.5));
        p.drawLine(QPointF(cx, 1), QPointF(cx, height() - 1));
    }
}

void AnimMiniTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    dragIndex_ = markAt(event->position());
    dragMoved_ = false;
    scrubbing_ = dragIndex_ < 0;
    if (dragIndex_ >= 0) {
        dragValue_ = marks_[static_cast<std::size_t>(dragIndex_)].time;
    } else if (onSeek) {
        onSeek(timeAt(event->position().x()));
    }
}

void AnimMiniTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton))
        return;
    if (scrubbing_) {
        if (onSeek)
            onSeek(timeAt(event->position().x()));
        return;
    }
    if (dragIndex_ < 0)
        return;
    const Mark& m = marks_[static_cast<std::size_t>(dragIndex_)];
    if (!dragMoved_ && std::abs(event->position().x() - x(m.time)) < 3.0)
        return;
    dragMoved_ = true;
    double value = timeAt(event->position().x());
    if (cursor_ >= from_ && std::abs(x(cursor_) - x(value)) <= kSnapPixels)
        value = cursor_;  // si aggancia al cursore
    dragValue_ = value;
    update();
}

void AnimMiniTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const int index = dragIndex_;
    const bool moved = dragMoved_;
    const double value = dragValue_;
    dragIndex_ = -1;
    dragMoved_ = false;
    scrubbing_ = false;
    if (index < 0 || index >= static_cast<int>(marks_.size())) {
        update();
        return;
    }
    const Mark m = marks_[static_cast<std::size_t>(index)];
    if (!moved) {
        if (onSeek)
            onSeek(m.time);  // un clic sul rombo porta il cursore sulla chiave
    } else if (std::abs(value - m.time) > kSame && onKeyMoved) {
        onKeyMoved(m.group, m.time, value);
    }
    update();
}

void AnimMiniTimeline::contextMenuEvent(QContextMenuEvent* event)
{
    const int index = markAt(event->pos());
    if (index < 0)
        return;
    const Mark m = marks_[static_cast<std::size_t>(index)];
    QMenu menu(this);
    struct Choice
    {
        const char* text;
        core::Interp interp;
    };
    const Choice choices[] = {{"Passaggio lineare", core::Interp::Linear}, {"Passaggio dolce", core::Interp::Smooth},
                              {"Passaggio fermo (salta)", core::Interp::Hold}};
    for (const Choice& c : choices) {
        QAction* a = menu.addAction(QString::fromUtf8(c.text));
        a->setCheckable(true);
        a->setChecked(m.interp == c.interp);
        a->setData(static_cast<int>(c.interp));
        a->setProperty("interp", true);
    }
    menu.addSeparator();
    QAction* remove = menu.addAction(QStringLiteral("Togli la chiave di %1").arg(QString::fromUtf8(animGroupName(m.group)).toLower()));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == nullptr)
        return;
    if (chosen == remove) {
        if (onKeyRemoved)
            onKeyRemoved(m.group, m.time);
    } else if (chosen->property("interp").toBool() && onKeyInterp) {
        onKeyInterp(m.group, m.time, static_cast<core::Interp>(chosen->data().toInt()));
    }
}

}  // namespace editor::app
