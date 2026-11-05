#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/AnimColors.h"
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

}  // namespace

void SequenceTimeline::setKeyRow(KeyRow row)
{
    const bool samePiece = row.layerPiece == keyRow_.layerPiece && row.id == keyRow_.id;
    bool same = samePiece && row.keys.size() == keyRow_.keys.size();
    for (std::size_t i = 0; same && i < row.keys.size(); ++i)
        same = row.keys[i].group == keyRow_.keys[i].group && sameInstant(row.keys[i].time, keyRow_.keys[i].time)
               && row.keys[i].interp == keyRow_.keys[i].interp;
    if (same)
        return;
    keyRow_ = std::move(row);
    const bool found = std::any_of(keyRow_.keys.begin(), keyRow_.keys.end(),
                                   [&](const KeyMark& k) { return selectedKey_ >= 0.0 && k.group == selectedKeyGroup_ && sameInstant(k.time, selectedKey_); });
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
    for (int i = 0; i < core::kAnimGroupCount; ++i) {
        const auto group = static_cast<core::AnimGroup>(i);
        if (std::any_of(keyRow_.keys.begin(), keyRow_.keys.end(), [&](const KeyMark& m) { return m.group == group; }))
            g.rows.push_back(group);
    }
    if (g.rows.empty())
        return g;
    // Una sotto-riga per gruppo: alta 6-11 pixel, tutte insieme al massimo poco più della metà del blocco.
    g.rowHeight = std::clamp(full.height() * 0.55 / static_cast<double>(g.rows.size()), 6.0, 11.0);
    const double h = std::min(full.height() - 2.0, g.rowHeight * static_cast<double>(g.rows.size()));
    g.rowHeight = h / static_cast<double>(g.rows.size());
    g.strip = QRectF(full.left(), full.bottom() - h, full.width(), h);
    g.ok = true;
    return g;
}

int SequenceTimeline::keyAt(const QPointF& pos) const
{
    const KeyGeometry g = keyGeometry();
    if (!g.ok || pos.y() < g.strip.top() - 2 || pos.y() > g.strip.bottom() + 1)
        return -1;
    int best = -1;
    double bestDistance = kReach + 1;
    for (int i = 0; i < static_cast<int>(keyRow_.keys.size()); ++i) {
        const KeyMark& mark = keyRow_.keys[static_cast<std::size_t>(i)];
        const double t = mark.time;
        if (t < g.source - kEdgeSlack || t > g.source + g.length + kEdgeSlack)
            continue;
        if (std::abs(pos.y() - keyY(g, mark.group)) > g.rowHeight / 2 + 1.5)
            continue;  // un'altra sotto-riga
        const double distance = std::abs(pos.x() - keyX(g, t));
        if (distance <= kReach && distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

double SequenceTimeline::keyY(const KeyGeometry& g, core::AnimGroup group) const
{
    const auto it = std::find(g.rows.begin(), g.rows.end(), group);
    const double rank = it == g.rows.end() ? 0.0 : static_cast<double>(it - g.rows.begin());
    return g.strip.top() + (rank + 0.5) * g.rowHeight;
}

QPointF SequenceTimeline::keyCenterForTest(int index) const
{
    const KeyGeometry g = keyGeometry();
    if (!g.ok || index < 0 || index >= static_cast<int>(keyRow_.keys.size()))
        return {-1000.0, -1000.0};
    const KeyMark& mark = keyRow_.keys[static_cast<std::size_t>(index)];
    return {keyX(g, mark.time), keyY(g, mark.group)};
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
    p.setBrush(QColor(0, 0, 0, 100));
    p.drawRect(strip);
    const QPalette pal = palette();
    const double half = std::min(5.0, g.rowHeight / 2 + 0.5);
    for (std::size_t row = 0; row < g.rows.size(); ++row) {
        const core::AnimGroup group = g.rows[row];
        const QColor color = animGroupColor(group);
        const double y = keyY(g, group);
        // La sotto-riga del gruppo: una fascia leggera nel suo colore e la linea che unisce le sue chiavi.
        QColor band = color;
        band.setAlpha(34);
        p.setPen(Qt::NoPen);
        p.setBrush(band);
        p.drawRect(QRectF(strip.left(), y - g.rowHeight / 2, strip.width(), g.rowHeight));
        QPolygonF line;
        for (int i = 0; i < static_cast<int>(keyRow_.keys.size()); ++i) {
            const KeyMark& k = keyRow_.keys[static_cast<std::size_t>(i)];
            if (k.group != group || k.time < g.source - kEdgeSlack || k.time > g.source + g.length + kEdgeSlack)
                continue;
            line << QPointF(keyX(g, keyDragging_ && i == keyDragIndex_ ? keyDragValue_ : k.time), y);
        }
        if (line.size() >= 2) {
            QColor stroke = color;
            stroke.setAlpha(190);
            p.setPen(QPen(stroke, 1.5));
            p.drawPolyline(line);
        }
    }
    for (int i = 0; i < static_cast<int>(keyRow_.keys.size()); ++i) {
        const KeyMark& k = keyRow_.keys[static_cast<std::size_t>(i)];
        if (k.time < g.source - kEdgeSlack || k.time > g.source + g.length + kEdgeSlack)
            continue;
        const bool dragged = keyDragging_ && i == keyDragIndex_;
        const double x = keyX(g, dragged ? keyDragValue_ : k.time);
        if (x < visibleLeft() - half || x > visibleRight() + half)
            continue;
        const double y = keyY(g, k.group);
        const bool selected = selectedKey_ >= 0.0 && k.group == selectedKeyGroup_ && sameInstant(k.time, selectedKey_);
        const QColor color = animGroupColor(k.group);
        p.setBrush(color);
        p.setPen(selected ? QPen(pal.color(QPalette::Highlight), 2.5) : QPen(QColor(15, 15, 15), 1.2));
        switch (k.interp) {
            case core::Interp::Hold:  // quadrato: salta
                p.drawRect(QRectF(x - half * 0.8, y - half * 0.8, half * 1.6, half * 1.6));
                break;
            case core::Interp::Smooth:  // tondo: parte e arriva piano
                p.drawEllipse(QPointF(x, y), half * 0.9, half * 0.9);
                break;
            case core::Interp::Linear:
            case core::Interp::Bezier: {  // rombo (la curva con un puntino scuro)
                QPolygonF diamond;
                diamond << QPointF(x, y - half) << QPointF(x + half, y) << QPointF(x, y + half) << QPointF(x - half, y);
                p.drawPolygon(diamond);
                if (k.interp == core::Interp::Bezier) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(20, 20, 20));
                    p.drawEllipse(QPointF(x, y), half * 0.28, half * 0.28);
                }
                break;
            }
        }
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
    const KeyMark& mark = keyRow_.keys[static_cast<std::size_t>(index)];
    const double time = mark.time;
    selectedKey_ = time;
    selectedKeyGroup_ = mark.group;
    invalidate();
    showKeyMenu(mark.group, time, event->globalPos());
    event->accept();
}

void SequenceTimeline::showKeyMenu(core::AnimGroup group, double fileTime, const QPoint& globalPos)
{
    core::Interp current = core::Interp::Linear;
    for (const KeyMark& k : keyRow_.keys) {
        if (k.group == group && sameInstant(k.time, fileTime))
            current = k.interp;
    }
    QMenu menu(this);
    menu.addSection(QString::fromUtf8(animGroupName(group)));
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
    QAction* remove = menu.addAction(QStringLiteral("Togli la chiave di %1").arg(QString::fromUtf8(animGroupName(group)).toLower()));
    QAction* chosen = menu.exec(globalPos);
    if (chosen == nullptr)
        return;
    if (chosen == remove) {
        if (onKeyRemoved)
            onKeyRemoved(group, fileTime);
        return;
    }
    if (chosen->property("interp").toBool() && onKeyInterp) {
        onKeyInterp(group, fileTime, static_cast<core::Interp>(chosen->data().toInt()), chosen->property("c1x").toDouble(),
                    chosen->property("c1y").toDouble(), chosen->property("c2x").toDouble(), chosen->property("c2y").toDouble());
    }
}

}  // namespace editor::app
