#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/SequenceTimelineShared.h"

#include "EditorCore/AudioTracks.h"
#include "EditorCore/LevelEnvelope.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Waveform.h"

#include <QtCore/QDir>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QHelpEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>

namespace editor::app {
using namespace detail;

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
    if (!blocks_.empty())
        paintCacheBar(p);
    if (fileDropX_ >= 0) {
        p.setRenderHint(QPainter::Antialiasing);
        if (fileDropLane_ > 0) {
            // File trascinati su una traccia aggiunta: dove inizieranno.
            const QRectF lane = audioLane(fileDropLane_);
            const double x = std::clamp(fileDropX_, lane.left() + 2, lane.right() - 2);
            p.setPen(QPen(palette().color(QPalette::Highlight), 4));
            p.drawLine(QPointF(x, lane.top() - 2), QPointF(x, lane.bottom() + 2));
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRectF(x + 6, lane.top(), 360, std::min(16.0, lane.height())), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("Lascia qui per mettere l'audio su %1").arg(lanes_[static_cast<std::size_t>(fileDropLane_)].name));
        } else {
            // File trascinati: dove andranno, fra i pezzi.
            const int index = dropIndexAt(fileDropX_);
            const double at = index > 0 ? startOfBlock(index - 1) + blocks_[static_cast<std::size_t>(index - 1)].length : 0.0;
            const QRectF lanes = videoLane().united(audioLane(0));
            const double x = blocks_.empty() ? lanes.left() + 2 : std::clamp(xForTime(at), lanes.left() + 2, lanes.right() - 2);
            p.setPen(QPen(palette().color(QPalette::Highlight), 4));
            p.drawLine(QPointF(x, lanes.top() - 4), QPointF(x, lanes.bottom() + 4));
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRectF(x + 6, lanes.top() - 15, 300, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("Lascia qui per mettere i file nel montaggio"));
        }
    }

    if (trimming_ && trimIndex_ >= 0 && trimIndex_ < static_cast<int>(blocks_.size())) {
        // Bordo trascinato: la parte aggiunta chiara, quella tolta scurita, e una riga dove sarà il bordo.
        const QRectF lane = videoLane();
        const double oldX = xForBlockEdge(trimIndex_, trimAtStart_);
        const double newX = xForTime(trimEdgeSequenceTime(trimValue_));
        const bool longer = trimAtStart_ ? newX < oldX : newX > oldX;
        const QRectF changed(QPointF(std::min(oldX, newX), lane.top()), QPointF(std::max(oldX, newX), lane.bottom()));
        p.fillRect(changed, longer ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 120));
        p.setPen(QPen(palette().color(QPalette::Highlight), 3));
        p.drawLine(QPointF(newX, lane.top() - 3), QPointF(newX, lane.bottom() + 3));
    }
    int trimLane = -1;
    if (audioTrimming_) {
        if (const AudioBlock* c = findAudioClip(audioTrimClip_, &trimLane)) {
            // Lo stesso per il bordo di una clip audio.
            const QRectF lane = audioLane(trimLane);
            const double oldEdge = audioTrimAtStart_ ? c->position : c->position + c->length;
            const double oldX = xForTime(oldEdge);
            const double newX = xForTime(oldEdge + audioTrimValue_ - (audioTrimAtStart_ ? c->sourceStart : c->sourceStart + c->length));
            const bool longer = audioTrimAtStart_ ? newX < oldX : newX > oldX;
            const QRectF changed(QPointF(std::min(oldX, newX), lane.top()), QPointF(std::max(oldX, newX), lane.bottom()));
            p.fillRect(changed, longer ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 120));
            p.setPen(QPen(palette().color(QPalette::Highlight), 3));
            p.drawLine(QPointF(newX, lane.top() - 3), QPointF(newX, lane.bottom() + 3));
        }
    }
    if (textTrimming_) {
        int trimLaneIndex = -1;
        if (const TextBlock* c = findTextClip(textTrimClip_, &trimLaneIndex)) {
            // Lo stesso per il bordo di una scritta.
            const QRectF lane = textLane(trimLaneIndex);
            const double oldEdge = textTrimAtStart_ ? c->position : c->position + c->length;
            const double oldX = xForTime(oldEdge);
            const double newX = xForTime(textTrimValue_);
            const bool longer = textTrimAtStart_ ? newX < oldX : newX > oldX;
            const QRectF changed(QPointF(std::min(oldX, newX), lane.top()), QPointF(std::max(oldX, newX), lane.bottom()));
            p.fillRect(changed, longer ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 120));
            p.setPen(QPen(palette().color(QPalette::Highlight), 3));
            p.drawLine(QPointF(newX, lane.top() - 3), QPointF(newX, lane.bottom() + 3));
        }
    }
    if (blocks_.empty() && lanes_.size() == 1 && textLanes_.empty())
        return;

    // Cursore, su tutte le corsie
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF a = videoLane();
    const QRectF last = audioLane(static_cast<int>(lanes_.size()) - 1);
    const double x = xForTime(playhead_);
    if (x < a.left() - 1 || x > a.right() + 1)
        return;  // con lo zoom il cursore può stare fuori dalla parte visibile
    const QColor head = palette().color(QPalette::Text);
    p.setPen(QPen(head, 2));
    p.drawLine(QPointF(x, 0), QPointF(x, last.bottom() + 3));
    QPainterPath triangle;  // in cima al righello
    triangle.moveTo(x - 5, 0);
    triangle.lineTo(x + 5, 0);
    triangle.lineTo(x, 7);
    triangle.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(head);
    p.drawPath(triangle);
}

void SequenceTimeline::paintStatic(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF a = videoLane();
    const QPalette pal = palette();
    mates_.clear();
    if (groupMembers) {
        for (const Object& o : allSelectedObjects()) {
            for (const Object& mate : groupMembers(o)) {
                if (std::find(mates_.begin(), mates_.end(), mate) == mates_.end())
                    mates_.push_back(mate);
            }
        }
    }
    // Un gruppo vero: c'è un compagno che non è un selezionato né il suo audio (o il suo pezzo video).
    const std::vector<Object> chosen = allSelectedObjects();
    matesGrouped_ = std::any_of(mates_.begin(), mates_.end(), [&](const Object& m) {
        return std::none_of(chosen.begin(), chosen.end(), [&](const Object& s) {
            const auto isPiece = [](Object::Kind k) { return k == Object::Kind::Piece || k == Object::Kind::PieceAudio; };
            return s == m || (s.id == m.id && isPiece(s.kind) && isPiece(m.kind));
        });
    });
    paintRuler(p);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(a, 4, 4);
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i)
        p.drawRoundedRect(audioLane(i), 4, 4);
    for (int i = 0; i < static_cast<int>(textLanes_.size()); ++i)
        p.drawRoundedRect(textLane(i), 4, 4);

    QFont small = p.font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);
    paintHeaders(p);

    if (blocks_.empty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(a, Qt::AlignCenter,
                   QStringLiteral("Sequenza vuota: scegli un file, segna I e O nell'anteprima di sinistra e premi A"));
    } else {
        paintVideo(p);
    }
    paintAudio(p);
    paintText(p);

    // La traccia scelta: contorno evidenziato attorno alla sua corsia.
    {
        const auto frame = [&](const QRectF& lane) {
            QColor tint = pal.color(QPalette::Highlight);
            tint.setAlpha(28);
            p.setPen(QPen(pal.color(QPalette::Highlight), 2));
            p.setBrush(tint);
            p.drawRoundedRect(lane.adjusted(1, 1, -1, -1), 4, 4);
        };
        if (videoLaneSelected())
            frame(videoLane());
        for (int i = 0; i < static_cast<int>(textLanes_.size()); ++i) {
            if (textLanes_[static_cast<std::size_t>(i)].layerId == selectedTextLayer_)
                frame(textLane(i));
        }
        for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
            if (audioLaneSelected(i))
                frame(audioLane(i));
        }
    }

    // Dove andrà la clip audio trascinata: il suo contorno sulla corsia di arrivo.
    if (audioDragging_ && audioDragLane_ >= 0) {
        if (const AudioBlock* c = findAudioClip(audioDragClip_)) {
            const QRectF lane = audioLane(audioDragLane_);
            const QRectF target(QPointF(xForTime(audioDragStart_) + 1, lane.top() + 1),
                                QPointF(xForTime(audioDragStart_ + c->length) - 1, lane.bottom() - 1));
            QColor fill = blockColor(c->colorIndex);
            fill.setAlpha(150);
            p.setBrush(fill);
            p.setPen(isMate(Object::Kind::AudioClip, c->clipId) ? QPen(QColor(255, 170, 0), 3) : QPen(pal.color(QPalette::Highlight), 3));
            p.drawRoundedRect(visiblePart(target), 3, 3);
            if (target.width() > 30) {
                p.setPen(Qt::white);
                p.drawText(visiblePart(target).adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                           p.fontMetrics().elidedText(QStringLiteral("%1  %2").arg(c->label, QString::fromStdString(core::formatDuration(audioDragStart_)).mid(3)),
                                                      Qt::ElideRight, static_cast<int>(target.width() - 8)));
            }
        }
    }

    // Dove andrà il blocco trascinato: il suo contorno, nel colore della selezione.
    if (dragging_ && dragIndex_ >= 0 && dragIndex_ < static_cast<int>(blocks_.size())) {
        const Block& b = blocks_[static_cast<std::size_t>(dragIndex_)];
        const QRectF target(QPointF(xForTime(dragStart_) + 1, a.top() + 1), QPointF(xForTime(dragStart_ + b.length) - 1, a.bottom() - 1));
        QColor fill = blockColor(b.colorIndex);
        fill.setAlpha(150);
        p.setBrush(fill);
        p.setPen(isMate(Object::Kind::Piece, b.fragmentId) ? QPen(QColor(255, 170, 0), 3) : QPen(pal.color(QPalette::Highlight), 3));
        p.drawRoundedRect(visiblePart(target), 3, 3);
        p.setPen(Qt::white);
        if (target.width() > 30)
            p.drawText(visiblePart(target).adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(QStringLiteral("%1\n%2").arg(b.label, QString::fromStdString(core::formatDuration(dragStart_)).mid(3)), Qt::ElideRight,
                                                  static_cast<int>(target.width() * 2)));
    }
}

void SequenceTimeline::paintVideo(QPainter& p)
{
    const QRectF a = videoLane();
    const QPalette pal = palette();
    // Blocchi: solo quelli che si vedono, e di ognuno solo la parte visibile.
    const std::vector<QRectF> rects = blockRects(a);
    p.save();
    p.setClipRect(a.adjusted(-1, -4, 1, 4));

    // Spazi vuoti: un riquadro tratteggiato (evidenziato se selezionato), con la durata se c'è posto.
    {
        double start = 0.0;
        for (const Block& b : blocks_) {
            if (b.gap > 0) {
                const QRectF full(QPointF(xForTime(start) + 1, a.top() + 2), QPointF(xForTime(start + b.gap) - 1, a.bottom() - 2));
                if (full.right() >= visibleLeft() - 4 && full.left() <= visibleRight() + 4 && full.width() >= 1) {
                    const QRectF r = visiblePart(full);
                    const bool selected = b.fragmentId == selectedGap_;
                    QColor fill = selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid);
                    fill.setAlpha(selected ? 70 : 40);
                    p.setBrush(fill);
                    p.setPen(QPen(selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid), selected ? 2 : 1, Qt::DashLine));
                    p.drawRoundedRect(r, 3, 3);
                    if (r.width() > 50) {
                        p.setPen(pal.color(QPalette::PlaceholderText));
                        const QString text = QStringLiteral("spazio vuoto\n%1").arg(
                            QString::fromStdString(core::formatDuration(b.gap)).mid(3));
                        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignCenter, p.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(r.width() * 2)));
                    }
                }
            }
            start += b.gap + b.length;
        }
    }
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
        p.setPen(isMate(Object::Kind::Piece, b.fragmentId)   ? QPen(QColor(255, 170, 0), 3)
                 : isSelected(Object::Kind::Piece, b.fragmentId) ? QPen(pal.color(QPalette::Highlight), 3)
                                                               : QPen(color.darker(140), 1));
        p.setBrush(Qt::NoBrush);
        // Il contorno spesso (scelto o di gruppo) sta tutto dentro il blocco, come quello delle clip audio.
        p.drawRoundedRect(p.pen().widthF() > 1.5 ? r.adjusted(1.5, 1.5, -1.5, -1.5) : r, 3, 3);
        // Segno in alto a destra: P = si copia con i suoi parametri, R = si ricodifica.
        if (b.fit != 0 && r.width() > 22) {
            const QRectF badge(QPointF(r.right() - 17, r.top() + 3), QSizeF(14, 14));
            p.setPen(Qt::NoPen);
            p.setBrush(b.fit == 1 ? QColor(232, 176, 44) : QColor(222, 62, 52));
            p.drawEllipse(badge);
            QFont bold = p.font();
            bold.setBold(true);
            p.save();
            p.setFont(bold);
            p.setPen(b.fit == 1 ? Qt::black : Qt::white);
            p.drawText(badge, Qt::AlignCenter, b.fit == 1 ? QStringLiteral("P") : QStringLiteral("R"));
            p.restore();
        }
        // Segno in alto a destra (a sinistra di P/R), celeste come la barretta della cache: l'anteprima legge dal proxy,
        // con la sua qualità.
        if (b.proxyDivisor > 1 && r.width() > 50) {
            QFont small = p.font();
            small.setBold(true);
            small.setPointSizeF(small.pointSizeF() * 0.85);
            p.save();
            p.setFont(small);
            const QString text = QStringLiteral("proxy 1/%1").arg(b.proxyDivisor);
            const QSizeF size(p.fontMetrics().horizontalAdvance(text) + 8, p.fontMetrics().height() + 1);
            const QRectF badge(QPointF(r.right() - (b.fit != 0 ? 20 : 3) - size.width(), r.top() + 3), size);
            if (badge.left() > r.left() + 3) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(110, 190, 240));
                p.drawRoundedRect(badge, 3, 3);
                p.setPen(Qt::black);
                p.drawText(badge, Qt::AlignCenter, text);
            }
            p.restore();
        }
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
}

void SequenceTimeline::paintHeaders(QPainter& p)
{
    const QPalette pal = palette();
    p.save();
    const auto label = [&](const QRectF& header, const QString& name, bool selected, bool dim) {
        p.setPen(Qt::NoPen);
        QColor fill = selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Button);
        if (selected)
            fill.setAlpha(150);
        p.setBrush(fill);
        p.drawRoundedRect(header, 4, 4);
        if (selected) {
            // La traccia scelta: barra piena sul bordo sinistro dell'intestazione.
            p.setBrush(pal.color(QPalette::Highlight));
            p.drawRoundedRect(QRectF(header.left(), header.top(), 5, header.height()), 2, 2);
        }
        QFont bold = p.font();
        bold.setBold(true);
        p.save();
        p.setFont(bold);
        p.setPen(dim ? pal.color(QPalette::PlaceholderText) : pal.color(QPalette::ButtonText));
        p.drawText(QRectF(header.left() + 4, header.top(), 24, header.height()), Qt::AlignLeft | Qt::AlignVCenter, name);
        p.restore();
    };
    label(headerRect(videoLane()), QStringLiteral("V1"), videoLaneSelected(), false);
    for (int i = 0; i < static_cast<int>(textLanes_.size()); ++i) {
        const TextLane& lane = textLanes_[static_cast<std::size_t>(i)];
        label(headerRect(textLane(i)), lane.name, lane.layerId == selectedTextLayer_, false);
    }
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const AudioLane& lane = lanes_[static_cast<std::size_t>(i)];
        label(headerRect(audioLane(i)), lane.name, audioLaneSelected(i), !lane.audible);
        // Muto e solo: quadratini accesi o spenti.
        const auto toggle = [&](const QRectF& box, const QString& text, bool on, const QColor& color) {
            p.setPen(QPen(on ? color.darker(130) : pal.color(QPalette::Mid), 1));
            p.setBrush(on ? color : pal.color(QPalette::Base));
            p.drawRoundedRect(box, 2, 2);
            p.setPen(on ? QColor(Qt::black) : pal.color(QPalette::ButtonText));
            p.drawText(box, Qt::AlignCenter, text);
        };
        toggle(muteBox(i), QStringLiteral("M"), lane.muted, QColor(230, 90, 70));
        toggle(soloBox(i), QStringLiteral("S"), lane.solo, QColor(240, 200, 60));
        // Livello: barretta riempita fino al valore, con una tacca a 0 dB e il valore scritto sopra.
        const QRectF bar = levelBar(i);
        if (bar.width() < 12)
            continue;
        const double db = levelLane_ == i ? levelValue_ : lane.levelDb;
        const double range = core::AudioTracks::kMaxLevelDb - core::AudioTracks::kMinLevelDb;
        const auto xOf = [&](double value) { return bar.left() + (value - core::AudioTracks::kMinLevelDb) / range * bar.width(); };
        p.setPen(QPen(pal.color(QPalette::Mid), 1));
        p.setBrush(pal.color(QPalette::Base));
        p.drawRoundedRect(bar, 2, 2);
        p.setPen(Qt::NoPen);
        p.setBrush(lane.audible ? QColor(90, 170, 110) : pal.color(QPalette::Mid));
        p.drawRoundedRect(QRectF(bar.topLeft(), QPointF(xOf(db), bar.bottom())), 2, 2);
        p.setPen(QPen(pal.color(QPalette::ButtonText), 1));
        p.drawLine(QPointF(xOf(0.0), bar.top()), QPointF(xOf(0.0), bar.bottom()));
        p.setPen(pal.color(QPalette::Text));
        const QString text = db == 0.0 ? QStringLiteral("0 dB") : QStringLiteral("%1 dB").arg(QString::number(db, 'f', db == std::round(db) ? 0 : 1));
        p.drawText(bar, Qt::AlignCenter, text);
    }
    p.restore();
}

void SequenceTimeline::paintText(QPainter& p)
{
    const QPalette pal = palette();
    const QColor base(150, 104, 205);
    p.save();
    for (int laneIndex = 0; laneIndex < static_cast<int>(textLanes_.size()); ++laneIndex) {
        const QRectF lane = textLane(laneIndex);
        p.setClipRect(lane.adjusted(-1, -3, 1, 3));
        for (const TextBlock& c : textLanes_[static_cast<std::size_t>(laneIndex)].clips) {
            const QRectF full(QPointF(xForTime(c.position) + 1, lane.top() + 1), QPointF(xForTime(c.position + c.length) - 1, lane.bottom() - 1));
            if (full.right() < visibleLeft() - 4 || full.left() > visibleRight() + 4)
                continue;
            const QRectF r = visiblePart(full);
            const bool dragged = textDragging_ && c.clipId == textDragClip_;
            QColor fill = base;
            if (dragged)
                fill.setAlpha(110);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(r, 3, 3);
            p.setPen(isMate(Object::Kind::TextClip, c.clipId)   ? QPen(QColor(255, 170, 0), 3)
                     : isSelected(Object::Kind::TextClip, c.clipId) ? QPen(pal.color(QPalette::Highlight), 3)
                                                                    : QPen(base.darker(140), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(p.pen().widthF() > 1.5 ? r.adjusted(1.5, 1.5, -1.5, -1.5) : r, 3, 3);
            if (r.width() > 20) {
                p.setPen(Qt::white);
                p.drawText(r.adjusted(5, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                           p.fontMetrics().elidedText(c.label, Qt::ElideRight, static_cast<int>(r.width() - 9)));
            }
        }
    }
    p.restore();

    // Dove andrà la scritta trascinata: il suo contorno sul livello di arrivo.
    if (textDragging_ && textDragLane_ >= 0) {
        if (const TextBlock* c = findTextClip(textDragClip_)) {
            const QRectF lane = textLane(textDragLane_);
            const QRectF target(QPointF(xForTime(textDragStart_) + 1, lane.top() + 1), QPointF(xForTime(textDragStart_ + c->length) - 1, lane.bottom() - 1));
            QColor fill = base;
            fill.setAlpha(150);
            p.setBrush(fill);
            p.setPen(isMate(Object::Kind::TextClip, c->clipId) ? QPen(QColor(255, 170, 0), 3) : QPen(pal.color(QPalette::Highlight), 3));
            p.drawRoundedRect(visiblePart(target), 3, 3);
            if (target.width() > 30) {
                p.setPen(Qt::white);
                p.drawText(visiblePart(target).adjusted(5, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                           p.fontMetrics().elidedText(c->label, Qt::ElideRight, static_cast<int>(target.width() - 9)));
            }
        }
    }
}

void SequenceTimeline::paintAudio(QPainter& p)
{
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const QRectF lane = audioLane(i);
        p.save();
        p.setClipRect(lane);
        if (i == 0)
            paintA1(p, lane);
        paintTrack(p, i, lane);
        // Una corsia che non si sente (muto, o non in solo quando altre lo sono): scurita.
        if (!lanes_[static_cast<std::size_t>(i)].audible) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 70));
            p.drawRect(lane);
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        if (i == 0) {
            for (std::size_t b = 0; b < blocks_.size(); ++b) {
                if (!blocks_[b].audioDetached)
                    paintEnvelope(p, targetOfBlock(b), lane);
            }
        }
        for (std::size_t c = 0; c < lanes_[static_cast<std::size_t>(i)].clips.size(); ++c)
            paintEnvelope(p, targetOfClip(i, c), lane);
        p.setRenderHint(QPainter::Antialiasing, false);
        // Tratto selezionato
        if (hasAudioSelection() && lanes_[static_cast<std::size_t>(i)].trackId == selectionTrack_) {
            const QRectF r = visiblePart(QRectF(QPointF(xForTime(selectionFrom_), lane.top()), QPointF(xForTime(selectionTo_), lane.bottom())));
            QColor highlight = palette().color(QPalette::Highlight);
            highlight.setAlpha(70);
            p.setPen(QPen(palette().color(QPalette::Highlight), 2));
            p.setBrush(highlight);
            p.drawRect(r);
        }
        p.restore();
    }
}

namespace {

constexpr double kEnvelopePad = 5.0;       // margine sopra e sotto la linea più alta e più bassa
constexpr double kEnvelopeNear = 5.0;      // distanza (pixel) entro cui si prende la linea
constexpr double kEnvelopePointNear = 7.0; // e un suo punto
constexpr double kEnvelopeSnapDb = 0.6;    // vicino a 0 dB ci si aggancia al centro

}  // namespace

double SequenceTimeline::envelopeY(const QRectF& lane, double db) const
{
    const double mid = lane.center().y();
    const double up = mid - (lane.top() + kEnvelopePad);
    const double down = (lane.bottom() - kEnvelopePad) - mid;
    if (db >= 0.0)
        return mid - std::min(db, core::AudioTracks::kMaxLevelDb) / core::AudioTracks::kMaxLevelDb * up;
    const double fraction = std::sqrt(std::min(-db, -core::AudioTracks::kMinLevelDb) / -core::AudioTracks::kMinLevelDb);
    return mid + fraction * down;
}

double SequenceTimeline::envelopeDbForY(const QRectF& lane, double y) const
{
    const double mid = lane.center().y();
    const double up = mid - (lane.top() + kEnvelopePad);
    const double down = (lane.bottom() - kEnvelopePad) - mid;
    double db = 0.0;
    if (y <= mid) {
        db = up > 0 ? std::min(1.0, (mid - y) / up) * core::AudioTracks::kMaxLevelDb : 0.0;
    } else {
        const double fraction = down > 0 ? std::min(1.0, (y - mid) / down) : 0.0;
        db = fraction >= 0.97 ? core::AudioTracks::kMinLevelDb : -fraction * fraction * -core::AudioTracks::kMinLevelDb;
    }
    return std::abs(db) < kEnvelopeSnapDb ? 0.0 : db;
}

SequenceTimeline::EnvTarget SequenceTimeline::targetOfBlock(std::size_t index) const
{
    const Block& b = blocks_[index];
    return {0, true, b.fragmentId, starts_[index], b.length, b.sourceStart, &b.envelope};
}

SequenceTimeline::EnvTarget SequenceTimeline::targetOfClip(int lane, std::size_t index) const
{
    const AudioBlock& c = lanes_[static_cast<std::size_t>(lane)].clips[index];
    return {lane, false, c.clipId, c.position, c.length, c.sourceStart, &c.envelope};
}

const std::vector<core::LevelPoint>& SequenceTimeline::lineOf(const EnvTarget& target) const
{
    return envActive_ && sameTarget(target, envTarget_) ? envPoints_ : *target.line;
}

bool SequenceTimeline::envelopeHit(const QPointF& pos, EnvTarget& target, int& point) const
{
    if (pos.x() < videoLane().left() || pos.x() > videoLane().right())
        return false;
    const int lane = laneAt(pos.y());
    if (lane < 0)
        return false;
    // Il pezzo o la clip sotto il mouse.
    const double t = timeForX(pos.x());
    EnvTarget found;
    bool any = false;
    const auto& clips = lanes_[static_cast<std::size_t>(lane)].clips;
    for (std::size_t i = 0; i < clips.size() && !any; ++i) {
        if (t >= clips[i].position && t <= clips[i].position + clips[i].length) {
            found = targetOfClip(lane, i);
            any = true;
        }
    }
    if (!any && lane == 0) {  // su A1, sotto le clip libere, l'audio dei pezzi video
        const int block = blockAt(pos.x());
        if (block >= 0 && !blocks_[static_cast<std::size_t>(block)].audioDetached && t >= starts_[static_cast<std::size_t>(block)]
            && t <= starts_[static_cast<std::size_t>(block)] + blocks_[static_cast<std::size_t>(block)].length) {
            found = targetOfBlock(static_cast<std::size_t>(block));
            any = true;
        }
    }
    if (!any)
        return false;
    const QRectF rect = audioLane(lane);
    const auto& line = lineOf(found);
    const auto atFile = [&](double fileTime) { return xForTime(found.position + (fileTime - found.source)); };
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i].time < found.source || line[i].time > found.source + found.length)
            continue;
        const QPointF at(atFile(line[i].time), envelopeY(rect, line[i].db));
        if (std::hypot(pos.x() - at.x(), pos.y() - at.y()) <= kEnvelopePointNear) {
            target = found;
            point = static_cast<int>(i);
            return true;
        }
    }
    const double fileTime = found.source + (t - found.position);
    if (std::abs(pos.y() - envelopeY(rect, core::envelopeDbAt(line, fileTime))) <= kEnvelopeNear) {
        target = found;
        point = -1;
        return true;
    }
    return false;
}

void SequenceTimeline::paintEnvelope(QPainter& p, const EnvTarget& target, const QRectF& lane)
{
    const double x0 = xForTime(target.position);
    const double x1 = xForTime(target.position + target.length);
    const double left = std::max(x0, visibleLeft());
    const double right = std::min(x1, visibleRight());
    if (right <= left)
        return;
    const auto& line = lineOf(target);
    const QColor accent(255, 190, 40);
    const auto fileTimeAt = [&](double x) { return target.source + (timeForX(x) - target.position); };
    const auto xAtFile = [&](double fileTime) { return xForTime(target.position + (fileTime - target.source)); };
    QPainterPath path;
    path.moveTo(left, envelopeY(lane, core::envelopeDbAt(line, fileTimeAt(left))));
    for (const core::LevelPoint& pt : line) {
        const double x = xAtFile(pt.time);
        if (x > left && x < right)
            path.lineTo(x, envelopeY(lane, pt.db));
    }
    path.lineTo(right, envelopeY(lane, core::envelopeDbAt(line, fileTimeAt(right))));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    if (line.empty()) {
        // Nessun punto: il livello è 0 dB dappertutto. La linea è appena accennata.
        QColor faint = accent;
        faint.setAlpha(110);
        p.setPen(QPen(faint, 1, Qt::DashLine));
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    }
    p.setPen(QPen(QColor(0, 0, 0, 150), 3.5));
    p.drawPath(path);
    p.setPen(QPen(accent, 1.8));
    p.drawPath(path);
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i].time < target.source || line[i].time > target.source + target.length)
            continue;
        const QPointF at(xAtFile(line[i].time), envelopeY(lane, line[i].db));
        if (at.x() < visibleLeft() - 8 || at.x() > visibleRight() + 8)
            continue;
        const bool taken = envActive_ && sameTarget(target, envTarget_) && static_cast<int>(i) == envPoint_;
        p.setPen(QPen(QColor(0, 0, 0, 200), 1.5));
        p.setBrush(taken ? accent : QColor(255, 255, 255));
        p.drawEllipse(at, taken ? 5.0 : 4.0, taken ? 5.0 : 4.0);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

void SequenceTimeline::paintA1(QPainter& p, const QRectF& lane)
{
    const QPalette pal = palette();
    if (blocks_.empty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(lane, Qt::AlignCenter, QStringLiteral("A1: l'audio dei pezzi video"));
        return;
    }
    // L'audio dei pezzi: la forma d'onda di ognuno, nel colore del suo file.
    const std::vector<QRectF> rects = blockRects(lane);
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        const QRectF& full = rects[i];
        if (b.audioDetached || full.right() < visibleLeft() - 4 || full.left() > visibleRight() + 4)
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
        const bool audioMate = isMate(Object::Kind::PieceAudio, b.fragmentId);
        if ((!multi_.empty() && isSelected(Object::Kind::PieceAudio, b.fragmentId)) || audioMate) {
            p.setPen(audioMate ? QPen(QColor(255, 170, 0), 3) : QPen(pal.color(QPalette::Highlight), 3));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 3, 3);
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
}

void SequenceTimeline::paintTrack(QPainter& p, int index, const QRectF& lane)
{
    const QPalette pal = palette();
    const AudioLane& track = lanes_[static_cast<std::size_t>(index)];
    if (track.clips.empty()) {
        if (index > 0) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(lane, Qt::AlignCenter, QStringLiteral("%1: trascina qui un file audio").arg(track.name));
        }
        return;
    }
    for (const AudioBlock& c : track.clips) {
        const QRectF full(QPointF(xForTime(c.position) + 1, lane.top() + 1), QPointF(xForTime(c.position + c.length) - 1, lane.bottom() - 1));
        if (full.right() < visibleLeft() - 4 || full.left() > visibleRight() + 4 || full.width() < 1)
            continue;
        const QRectF r = visiblePart(full);
        QColor color = blockColor(c.colorIndex);
        const bool moving = audioDragging_ && c.clipId == audioDragClip_;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(color.red(), color.green(), color.blue(), moving ? 40 : 90));
        p.drawRoundedRect(r, 3, 3);
        if (c.waveform && !c.waveform->empty())
            drawWave(p, full.adjusted(0, 2, 0, -2), *c.waveform, c.sourceStart, c.sourceStart + c.length, color.darker(moving ? 100 : 140),
                     visibleLeft(), visibleRight());
        else if (!c.waveform && r.width() > 80) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("forma d'onda..."));
        }
        p.setPen(isMate(Object::Kind::AudioClip, c.clipId)   ? QPen(QColor(255, 170, 0), 3)
                 : isSelected(Object::Kind::AudioClip, c.clipId) ? QPen(pal.color(QPalette::Highlight), 3)
                                                               : QPen(color.darker(140), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 3, 3);
        if (r.width() > 40) {
            const QString text = QStringLiteral("♪ %1  %2").arg(c.label, QString::fromStdString(core::formatDuration(c.length)).mid(3));
            const QRectF band(QPointF(r.left() + 3, r.top() + 2), QSizeF(r.width() - 6, std::min(r.height() - 4, p.fontMetrics().height() + 0.0)));
            p.setPen(pal.color(QPalette::Text));
            p.drawText(band, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(band.width())));
        }
    }
}
}  // namespace editor::app
