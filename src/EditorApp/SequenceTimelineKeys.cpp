#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/SequenceTimelineShared.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QPainter>
#include <QtGui/QPolygonF>
#include <QtWidgets/QMenu>

#include <algorithm>
#include <cmath>

// La riga dei rombi delle chiavi di animazione sul pezzo scelto (vedi SequenceTimeline::KeyRow).

namespace editor::app {
using namespace detail;

namespace {

constexpr double kDiamond = 5.5;   // mezza diagonale del rombo
constexpr double kReach = 7.0;     // pixel entro cui si prende un rombo
constexpr double kEdgeSlack = 1e-6;

bool sameInstant(double a, double b)
{
    return std::abs(a - b) <= 1e-6;
}

QColor keyColor(core::Interp interp)
{
    switch (interp) {
        case core::Interp::Hold: return QColor(240, 160, 40);
        case core::Interp::Smooth: return QColor(120, 190, 255);
        case core::Interp::Bezier: return QColor(120, 220, 120);
        case core::Interp::Linear: break;
    }
    return QColor(255, 255, 255);
}

}  // namespace

void SequenceTimeline::setKeyRow(KeyRow row)
{
    const bool samePiece = row.layerPiece == keyRow_.layerPiece && row.id == keyRow_.id;
    bool same = samePiece && row.keys.size() == keyRow_.keys.size();
    for (std::size_t i = 0; same && i < row.keys.size(); ++i)
        same = sameInstant(row.keys[i].time, keyRow_.keys[i].time) && row.keys[i].interp == keyRow_.keys[i].interp;
    if (same)
        return;
    keyRow_ = std::move(row);
    const bool found = std::any_of(keyRow_.keys.begin(), keyRow_.keys.end(),
                                   [&](const KeyMark& k) { return selectedKey_ >= 0.0 && sameInstant(k.time, selectedKey_); });
    if (!samePiece || !found)
        selectedKey_ = -1.0;
    keyDragIndex_ = -1;
    keyDragging_ = false;
    invalidate();
}

SequenceTimeline::KeyGeometry SequenceTimeline::keyGeometry() const
{
    KeyGeometry g;
    if (keyRow_.keys.empty())
        return g;
    QRectF full;
    if (!keyRow_.layerPiece) {
        const std::vector<QRectF> rects = blockRects(videoLane());
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            if (blocks_[i].fragmentId != keyRow_.id)
                continue;
            full = rects[i];
            g.position = starts_[i];
            g.length = blocks_[i].length;
            g.source = blocks_[i].sourceStart;
            break;
        }
    } else {
        int lane = 0;
        const TextBlock* c = findTextClip(keyRow_.id, &lane);
        if (c == nullptr || !c->piece)
            return g;
        const QRectF l = textLane(lane);
        full = QRectF(QPointF(xForTime(c->position) + 1, l.top() + 1), QPointF(xForTime(c->position + c->length) - 1, l.bottom() - 1));
        g.position = c->position;
        g.length = c->length;
        g.source = c->sourceStart;
    }
    if (full.isEmpty())
        return g;
    const double h = std::min(14.0, full.height() * 0.5);
    g.strip = QRectF(full.left(), full.bottom() - h, full.width(), h);
    g.ok = true;
    return g;
}

int SequenceTimeline::keyAt(const QPointF& pos) const
{
    const KeyGeometry g = keyGeometry();
    if (!g.ok || pos.y() < g.strip.top() - 3 || pos.y() > g.strip.bottom() + 1)
        return -1;
    int best = -1;
    double bestDistance = kReach + 1;
    for (int i = 0; i < static_cast<int>(keyRow_.keys.size()); ++i) {
        const double t = keyRow_.keys[static_cast<std::size_t>(i)].time;
        if (t < g.source - kEdgeSlack || t > g.source + g.length + kEdgeSlack)
            continue;
        const double distance = std::abs(pos.x() - keyX(g, t));
        if (distance <= kReach && distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

QPointF SequenceTimeline::keyCenterForTest(int index) const
{
    const KeyGeometry g = keyGeometry();
    if (!g.ok || index < 0 || index >= static_cast<int>(keyRow_.keys.size()))
        return {-1000.0, -1000.0};
    return {keyX(g, keyRow_.keys[static_cast<std::size_t>(index)].time), g.strip.center().y()};
}

void SequenceTimeline::paintKeyRow(QPainter& p)
{
    const KeyGeometry g = keyGeometry();
    if (!g.ok)
        return;
    p.save();
    p.setClipRect(QRectF(QPointF(videoLane().left(), 0), QPointF(videoLane().right(), height())));
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF strip = visiblePart(g.strip);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRect(strip);
    const QPalette pal = palette();
    for (int i = 0; i < static_cast<int>(keyRow_.keys.size()); ++i) {
        const KeyMark& k = keyRow_.keys[static_cast<std::size_t>(i)];
        if (k.time < g.source - kEdgeSlack || k.time > g.source + g.length + kEdgeSlack)
            continue;
        const bool dragged = keyDragging_ && i == keyDragIndex_;
        const double time = dragged ? keyDragValue_ : k.time;
        const double x = keyX(g, time);
        if (x < visibleLeft() - kDiamond || x > visibleRight() + kDiamond)
            continue;
        const double y = g.strip.center().y();
        const double half = std::min(kDiamond, g.strip.height() / 2 + 1);
        QPolygonF diamond;
        diamond << QPointF(x, y - half) << QPointF(x + half, y) << QPointF(x, y + half) << QPointF(x - half, y);
        const bool selected = selectedKey_ >= 0.0 && sameInstant(k.time, selectedKey_);
        p.setBrush(keyColor(k.interp));
        p.setPen(selected ? QPen(pal.color(QPalette::Highlight), 2.5) : QPen(QColor(20, 20, 20), 1.2));
        p.drawPolygon(diamond);
    }
    p.restore();
}

void SequenceTimeline::contextMenuEvent(QContextMenuEvent* event)
{
    const int index = keyAt(event->pos());
    if (index < 0) {
        event->ignore();
        return;
    }
    const double time = keyRow_.keys[static_cast<std::size_t>(index)].time;
    selectedKey_ = time;
    invalidate();
    showKeyMenu(time, event->globalPos());
    event->accept();
}

void SequenceTimeline::showKeyMenu(double fileTime, const QPoint& globalPos)
{
    core::Interp current = core::Interp::Linear;
    for (const KeyMark& k : keyRow_.keys) {
        if (sameInstant(k.time, fileTime))
            current = k.interp;
    }
    QMenu menu(this);
    const auto addInterp = [&](QMenu& into, const QString& text, core::Interp interp, std::array<double, 4> c, bool checked) {
        QAction* action = into.addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        action->setData(static_cast<int>(interp));
        action->setProperty("c1x", c[0]);
        action->setProperty("c1y", c[1]);
        action->setProperty("c2x", c[2]);
        action->setProperty("c2y", c[3]);
        action->setProperty("interp", true);
        return action;
    };
    addInterp(menu, QStringLiteral("Lineare (velocità costante)"), core::Interp::Linear, {0, 0, 1, 1}, current == core::Interp::Linear);
    addInterp(menu, QStringLiteral("Dolce (parte e arriva piano)"), core::Interp::Smooth, {0.42, 0, 0.58, 1}, current == core::Interp::Smooth);
    addInterp(menu, QStringLiteral("Fermo (salta alla chiave dopo)"), core::Interp::Hold, {0, 0, 1, 1}, current == core::Interp::Hold);
    QMenu* bezier = menu.addMenu(QStringLiteral("Curva"));
    addInterp(*bezier, QStringLiteral("Parte piano, poi veloce"), core::Interp::Bezier, {0.42, 0, 1, 1}, false);
    addInterp(*bezier, QStringLiteral("Parte veloce, arriva piano"), core::Interp::Bezier, {0, 0, 0.58, 1}, false);
    addInterp(*bezier, QStringLiteral("Molto dolce"), core::Interp::Bezier, {0.65, 0, 0.35, 1}, false);
    addInterp(*bezier, QStringLiteral("Rimbalzo in arrivo"), core::Interp::Bezier, {0.34, 1.56, 0.64, 1}, false);
    menu.addSeparator();
    QAction* remove = menu.addAction(QStringLiteral("Togli la chiave"));
    QAction* chosen = menu.exec(globalPos);
    if (chosen == nullptr)
        return;
    if (chosen == remove) {
        if (onKeyRemoved)
            onKeyRemoved(fileTime);
        return;
    }
    if (chosen->property("interp").toBool() && onKeyInterp) {
        onKeyInterp(fileTime, static_cast<core::Interp>(chosen->data().toInt()), chosen->property("c1x").toDouble(),
                    chosen->property("c1y").toDouble(), chosen->property("c2x").toDouble(), chosen->property("c2y").toDouble());
    }
}

}  // namespace editor::app
