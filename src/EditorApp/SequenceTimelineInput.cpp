#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/SequenceTimelineShared.h"
#include "EditorApp/TransitionMime.h"

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

void SequenceTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    pressX_ = pos.x();
    pressY_ = pos.y();
    const double t = timeForX(pressX_);
    const bool content = !blocks_.empty() || lanes_.size() > 1 || !textLanes_.empty();
    // Ctrl+clic su un oggetto: lo aggiunge alla selezione o lo toglie (nient'altro: niente spostamento del cursore).
    if ((event->modifiers() & Qt::ControlModifier) && pos.x() >= videoLane().left() && pos.y() >= lanesTop() - 2 && ctrlPress(pos))
        return;
    if (selectedTransition_ != 0) {  // un clic altrove toglie la selezione della transizione (se è lei, si rimette sotto)
        selectedTransition_ = 0;
        invalidate();
    }
    if (!multi_.empty()) {  // un clic senza Ctrl riparte da un oggetto solo
        multi_.clear();
        invalidate();
        if (onMultiSelection)
            onMultiSelection();
    }

    // Livelli di scritte, sopra la corsia video: il nome, una scritta (bordi o spostamento) o lo sfondo.
    if (const int textIndex = textLaneAt(pos.y()); textIndex >= 0) {
        const TextLane& textLane = textLanes_[static_cast<std::size_t>(textIndex)];
        if (pos.x() < videoLane().left() - 2) {
            setSelectedTextLayer(textLane.layerId);
            if (onTextClipSelected)
                onTextClipSelected(0);
            return;
        }
        int clipLane = 0;
        int clip = 0;
        bool atStart = false;
        if (textFadeHandleAt(pos, clipLane, clip, atStart)) {
            // Una maniglia della dissolvenza (atStart qui vuol dire "in entrata").
            const TextBlock& c = textLanes_[static_cast<std::size_t>(clipLane)].clips[static_cast<std::size_t>(clip)];
            textFadeClip_ = c.clipId;
            textFadeIsIn_ = atStart;
            textFading_ = false;
            textFadeValue_ = atStart ? c.fadeIn : c.fadeOut;
            setSelectedTextClip(c.clipId);
            if (onTextClipSelected)
                onTextClipSelected(c.clipId);
            return;
        }
        if (textEdgeAt(pos, clipLane, clip, atStart)) {
            const TextBlock& c = textLanes_[static_cast<std::size_t>(clipLane)].clips[static_cast<std::size_t>(clip)];
            textTrimClip_ = c.clipId;
            textTrimAtStart_ = atStart;
            textTrimming_ = false;
            textTrimValue_ = atStart ? c.position : c.position + c.length;
            textTrimGrab_ = t - textTrimValue_;
            setSelectedTextClip(c.clipId);
            if (onTextClipSelected)
                onTextClipSelected(c.clipId);
            return;
        }
        if (textClipAt(pos, clipLane, clip)) {
            const TextBlock& c = textLanes_[static_cast<std::size_t>(clipLane)].clips[static_cast<std::size_t>(clip)];
            textDragClip_ = c.clipId;
            textDragging_ = false;
            textDragGrab_ = t - c.position;
            textDragStart_ = c.position;
            textDragLane_ = clipLane;
            setSelectedTextClip(c.clipId);
            playhead_ = std::max(0.0, t);
            if (onTextClipSelected)
                onTextClipSelected(c.clipId);
            if (onSeek)
                onSeek(playhead_);
            return;
        }
        // Lo sfondo del livello: sceglie il livello e porta il cursore lì.
        setSelectedTextLayer(textLane.layerId);
        if (onTextClipSelected)
            onTextClipSelected(0);
        playhead_ = std::max(0.0, t);
        if (onSeek)
            onSeek(playhead_);
        return;
    }
    if (pos.y() >= lanesTop() - 2)
        clearTextSelection();  // il righello non cambia la selezione

    // A sinistra delle corsie: nome, muto, solo e livello delle corsie audio.
    if (pos.x() < videoLane().left() - 2) {
        const int lane = laneAt(pos.y());
        if (lane < 0)
            return;
        const AudioLane& l = lanes_[static_cast<std::size_t>(lane)];
        if (muteBox(lane).contains(pos)) {
            if (onTrackMuted)
                onTrackMuted(l.trackId, !l.muted);
        } else if (soloBox(lane).contains(pos)) {
            if (onTrackSolo)
                onTrackSolo(l.trackId, !l.solo);
        } else if (levelBar(lane).contains(pos)) {
            levelLane_ = lane;
            levelValue_ = levelForX(lane, pos.x());
            invalidate();
        } else {
            setSelectedTrack(l.trackId);
        }
        return;
    }
    if (pos.y() < videoLane().top() - 2) {
        if (!content)
            return;
        // Righello: sposta il cursore, anche trascinando.
        scrubbing_ = true;
        playhead_ = std::clamp(t, 0.0, total_);
        update();
        if (onSeek)
            onSeek(playhead_);
        return;
    }
    if (pos.y() >= videoLane().top() && pos.y() <= videoLane().bottom()) {
        // Una transizione sulla corsia video: un clic la sceglie, un bordo ne cambia la durata.
        bool edge = false;
        bool atStart = false;
        if (const int index = transitionAt(pos, edge, atStart); index >= 0) {
            const TransitionBlock& transition = transitions_[static_cast<std::size_t>(index)];
            const int id = transition.id;
            setSelectedTransition(id);
            transTrimId_ = edge ? id : 0;
            transTrimming_ = false;
            transTrimDuration_ = transition.end - transition.start;
            if (!edge) {
                playhead_ = std::clamp(t, 0.0, total_);
                if (onSeek)
                    onSeek(playhead_);
            }
            if (onTransitionSelected)
                onTransitionSelected(id);
            return;
        }
    }
    if (const int lane = laneAt(pos.y()); lane >= 0) {
        // Corsie audio: la linea del livello, le clip delle tracce aggiunte (bordi o spostamento), altrimenti la
        // selezione di un tratto.
        selected_ = 0;
        selectedGap_ = 0;
        setSelectedTrack(lanes_[static_cast<std::size_t>(lane)].trackId);
        if (JunctionDot dot; junctionHit(pos, dot)) {
            // Il pallino della giunzione (o la giunzione, per ricrearlo): si trascina in verticale.
            const QRectF rect = audioLane(lane);
            jointActive_ = true;
            joint_ = dot;
            jointBaseFirst_ = *dot.first.line;
            jointBaseSecond_ = *dot.second.line;
            jointMoved_ = false;
            jointDb_ = dot.level ? *dot.level
                                 : (std::abs(pos.y() - envelopeY(rect, dot.firstEdgeDb)) <= std::abs(pos.y() - envelopeY(rect, dot.secondEdgeDb))
                                        ? dot.firstEdgeDb
                                        : dot.secondEdgeDb);
            updateJointLines();
            invalidate();
            return;
        }
        EnvTarget envTarget;
        int envPoint = -1;
        if (envelopeHit(pos, envTarget, envPoint)) {
            envActive_ = true;
            envTarget_ = envTarget;
            envPoints_ = *envTarget.line;
            envMoved_ = false;
            if (envPoint < 0) {
                // Un punto nuovo, sulla linea dove si è cliccato (tempo del file, dentro il pezzo o la clip).
                const double fileTime = envTarget.source + std::clamp(t - envTarget.position, 0.0, envTarget.length);
                const core::LevelPoint added{fileTime, core::envelopeDbAt(envPoints_, fileTime)};
                const auto at = std::upper_bound(envPoints_.begin(), envPoints_.end(), added.time,
                                                 [](double time, const core::LevelPoint& q) { return time < q.time; });
                envPoint = static_cast<int>(at - envPoints_.begin());
                envPoints_.insert(at, added);
                envMoved_ = true;
            }
            envPoint_ = envPoint;
            invalidate();
            return;
        }
        int clipLane = 0;
        int clip = 0;
        bool atStart = false;
        if (audioEdgeAt(pos, clipLane, clip, atStart)) {
            const AudioBlock& c = lanes_[static_cast<std::size_t>(clipLane)].clips[static_cast<std::size_t>(clip)];
            audioTrimClip_ = c.clipId;
            audioTrimAtStart_ = atStart;
            audioTrimming_ = false;
            audioTrimValue_ = atStart ? c.sourceStart : c.sourceStart + c.length;
            audioTrimGrab_ = t - (atStart ? c.position : c.position + c.length);
            setSelectedAudioClip(c.clipId);
            if (onAudioClipSelected)
                onAudioClipSelected(c.clipId);
            return;
        }
        if (audioClipAt(pos, clipLane, clip)) {
            const AudioBlock& c = lanes_[static_cast<std::size_t>(clipLane)].clips[static_cast<std::size_t>(clip)];
            audioDragClip_ = c.clipId;
            audioDragging_ = false;
            audioDragGrab_ = t - c.position;
            audioDragStart_ = c.position;
            audioDragLane_ = clipLane;
            setSelectedAudioClip(c.clipId);
            playhead_ = std::max(0.0, t);
            if (onAudioClipSelected)
                onAudioClipSelected(c.clipId);
            if (onSeek)
                onSeek(playhead_);
            return;
        }
        selectedAudioClip_ = 0;
        if (onAudioClipSelected)
            onAudioClipSelected(0);
        // L'audio di un pezzo video (A1) è del pezzo: prenderlo e trascinarlo sposta il pezzo (e il suo gruppo).
        if (lanes_[static_cast<std::size_t>(lane)].trackId == core::AudioTracks::kOriginalTrack) {
            const int index = blockAt(pos.x());
            if (index >= 0 && !blocks_[static_cast<std::size_t>(index)].audioDetached && t >= starts_[static_cast<std::size_t>(index)]
                && t < starts_[static_cast<std::size_t>(index)] + blocks_[static_cast<std::size_t>(index)].length) {
                dragIndex_ = index;
                dragging_ = false;
                selected_ = blocks_[static_cast<std::size_t>(index)].fragmentId;
                dragGrab_ = t - starts_[static_cast<std::size_t>(index)];
                dragStart_ = starts_[static_cast<std::size_t>(index)];
                if (onSelect)
                    onSelect(selected_);
                playhead_ = std::max(0.0, t);
                invalidate();
                if (onSeek)
                    onSeek(playhead_);
                return;
            }
        }
        selectionAnchor_ = std::max(0.0, t);
        // Su A1 (l'audio dei video) il trascinamento non seleziona un tratto: il muto si fa con la linea del livello.
        if (lanes_[static_cast<std::size_t>(lane)].trackId != core::AudioTracks::kOriginalTrack) {
            selecting_ = true;
            setAudioSelection(selectionAnchor_, selectionAnchor_, lanes_[static_cast<std::size_t>(lane)].trackId);
        }
        playhead_ = selectionAnchor_;
        if (onSeek)
            onSeek(selectionAnchor_);
        return;
    }
    if (blocks_.empty() || pos.y() > videoLane().bottom() + kLaneGap / 2)
        return;

    selectedAudioClip_ = 0;
    selectedTrack_ = -1;  // si sceglie un pezzo della traccia video
    syncTrackButtons();
    // Vicino al bordo di un blocco: si sposta il bordo (inizio o fine del pezzo nel file).
    bool atStart = false;
    if (const int edge = edgeAt(pos, atStart); edge >= 0) {
        trimIndex_ = edge;
        trimAtStart_ = atStart;
        trimming_ = false;
        const Block& b = blocks_[static_cast<std::size_t>(edge)];
        trimValue_ = atStart ? b.sourceStart : b.sourceStart + b.length;
        trimGrab_ = t - (atStart ? starts_[static_cast<std::size_t>(edge)] : starts_[static_cast<std::size_t>(edge)] + b.length);
        selected_ = b.fragmentId;
        selectedGap_ = 0;
        if (onSelect)
            onSelect(selected_);
        invalidate();
        return;
    }

    dragIndex_ = blockAt(pressX_);
    dragging_ = false;
    if (dragIndex_ >= 0) {
        selected_ = blocks_[dragIndex_].fragmentId;
        selectedGap_ = 0;
        dragGrab_ = t - startOfBlock(dragIndex_);
        dragStart_ = startOfBlock(dragIndex_);
        if (onSelect)
            onSelect(selected_);
    } else if (const int gap = gapAt(pressX_); gap >= 0) {
        selectedGap_ = blocks_[gap].fragmentId;
        selected_ = 0;
        if (onSelect)
            onSelect(0);
    }
    playhead_ = t;
    invalidate();
    if (onSeek)
        onSeek(playhead_);
}

void SequenceTimeline::leaveEvent(QEvent* event)
{
    if (textHoverClip_ != 0) {
        textHoverClip_ = 0;
        update();
    }
    ZoomableTimeline::leaveEvent(event);
}

void SequenceTimeline::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const double x = pos.x();
    if (!(event->buttons() & Qt::LeftButton)) {
        // Sul bordo di un blocco o di una clip audio, o sul livello di una corsia, il cursore diventa ⟷.
        bool atStart = false;
        int lane = 0;
        int clip = 0;
        const int header = x < videoLane().left() ? laneAt(pos.y()) : -1;
        EnvTarget envTarget;
        int envPoint = -1;
        bool fadeIn = false;
        // Scritte: sulla maniglia in alto (dissolvenza) il cursore è una mano e la maniglia si evidenzia; sul bordo sotto
        // (accorcia) è ⟷.
        int hoverClip = 0;
        bool transitionEdge = false;
        if (JunctionDot dot; junctionHit(pos, dot))
            setCursor(Qt::SizeVerCursor);
        else if (envelopeHit(pos, envTarget, envPoint))
            setCursor(envPoint >= 0 ? Qt::SizeAllCursor : Qt::SizeVerCursor);
        else if (transitionAt(pos, transitionEdge, atStart) >= 0 && transitionEdge)
            setCursor(Qt::SizeHorCursor);
        else if (textFadeHandleAt(pos, lane, clip, fadeIn)) {
            setCursor(Qt::PointingHandCursor);
            hoverClip = textLanes_[static_cast<std::size_t>(lane)].clips[static_cast<std::size_t>(clip)].clipId;
        } else if (edgeAt(pos, atStart) >= 0 || audioEdgeAt(pos, lane, clip, atStart) || textEdgeAt(pos, lane, clip, atStart) || (header >= 0 && levelBar(header).contains(pos)))
            setCursor(Qt::SizeHorCursor);
        else
            setCursor(Qt::ArrowCursor);
        if (hoverClip != textHoverClip_ || (hoverClip != 0 && fadeIn != textHoverIn_)) {
            textHoverClip_ = hoverClip;
            textHoverIn_ = hoverClip != 0 && fadeIn;
            update();
        }
        return;
    }
    if (jointActive_) {
        // Il pallino comune segue il mouse solo in verticale: sta sulla giunzione.
        if (jointMoved_ || std::abs(x - pressX_) >= 2 || std::abs(pos.y() - pressY_) >= 2) {
            jointDb_ = envelopeDbForY(audioLane(joint_.first.lane), pos.y());
            jointMoved_ = true;
            updateJointLines();
        }
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1: %2 dB (giunzione)").arg(lanes_[static_cast<std::size_t>(joint_.first.lane)].name,
                                                                         QString::number(jointDb_, 'f', 1)),
                           this);
        invalidate();
        return;
    }
    if (envActive_ && envPoint_ >= 0) {
        // Il punto segue il mouse, ma resta dentro il pezzo o la clip e fra i due vicini.
        auto& point = envPoints_[static_cast<std::size_t>(envPoint_)];
        const double previous = envPoint_ > 0 ? envPoints_[static_cast<std::size_t>(envPoint_) - 1].time + 1e-3 : 0.0;
        const double next = envPoint_ + 1 < static_cast<int>(envPoints_.size())
                                ? envPoints_[static_cast<std::size_t>(envPoint_) + 1].time - 1e-3
                                : std::numeric_limits<double>::max();
        if (envMoved_ || std::abs(x - pressX_) >= 2 || std::abs(pos.y() - pressY_) >= 2) {
            const double fileTime = envTarget_.source + std::clamp(timeForX(x) - envTarget_.position, 0.0, envTarget_.length);
            point.time = std::clamp(fileTime, previous, std::max(previous, next));
            point.db = envelopeDbForY(audioLane(envTarget_.lane), pos.y());
            envMoved_ = true;
        }
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1: %2 dB a %3").arg(lanes_[static_cast<std::size_t>(envTarget_.lane)].name,
                                                                  QString::number(point.db, 'f', 1),
                                                                  QString::fromStdString(core::formatDuration(
                                                                      envTarget_.position + point.time - envTarget_.source))),
                           this);
        invalidate();
        return;
    }
    if (levelLane_ >= 0) {
        levelValue_ = levelForX(levelLane_, x);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1: %2 dB").arg(lanes_[static_cast<std::size_t>(levelLane_)].name, QString::number(levelValue_, 'f', 1)),
                           this);
        invalidate();
        return;
    }
    if (audioTrimClip_ != 0) {
        if (!audioTrimming_ && std::abs(x - pressX_) < 2)
            return;
        audioTrimming_ = true;
        audioTrimRipple_ = (event->modifiers() & Qt::AltModifier) != 0;
        audioTrimValue_ = audioTrimTarget(x, audioTrimRipple_);
        if (const AudioBlock* c = findAudioClip(audioTrimClip_)) {
            const double before = audioTrimAtStart_ ? c->sourceStart : c->sourceStart + c->length;
            const double delta = audioTrimValue_ - before;
            QToolTip::showText(event->globalPosition().toPoint(),
                               QStringLiteral("%1 %2 (%3%4 s)%5")
                                   .arg(audioTrimAtStart_ ? QStringLiteral("Inizio") : QStringLiteral("Fine"),
                                        QString::fromStdString(core::formatDuration(audioTrimValue_)),
                                        delta >= 0 ? QStringLiteral("+") : QString(), QString::number(delta, 'f', 3),
                                        audioTrimRipple_ ? QStringLiteral("\nAlt: le clip dopo scorrono")
                                                         : QStringLiteral("\nCon Alt le clip dopo sulla traccia scorrono")),
                               this);
        }
        update();
        return;
    }
    if (audioDragClip_ != 0) {
        if (!audioDragging_ && std::abs(x - pressX_) <= 8 && std::abs(pos.y() - pressY_) <= 8)
            return;
        if (!audioDragging_) {
            audioDragging_ = true;
            setCursor(Qt::ClosedHandCursor);
        }
        // In verticale si cambia traccia (anche A1: la clip resta staccata dai pezzi video).
        if (const int lane = laneAt(pos.y()); lane >= 0)
            audioDragLane_ = lane;
        audioDragStart_ = audioDragTarget(x, audioDragLane_);
        invalidate();
        return;
    }
    if (textFadeClip_ != 0) {
        if (!textFading_ && std::abs(x - pressX_) < 2)
            return;
        if (const TextBlock* c = findTextClip(textFadeClip_)) {
            textFading_ = true;
            // La maniglia segue il mouse: l'entrata dal principio della scritta, l'uscita dalla fine.
            const double t = timeForX(x);
            const double wanted = textFadeIsIn_ ? t - c->position : c->position + c->length - t;
            const double other = textFadeIsIn_ ? c->fadeOut : c->fadeIn;
            textFadeValue_ = std::clamp(wanted, 0.0, std::max(0.0, c->length - std::clamp(other, 0.0, c->length)));
            QToolTip::showText(event->globalPosition().toPoint(),
                               QStringLiteral("Dissolvenza in %1: %2 s").arg(textFadeIsIn_ ? QStringLiteral("entrata") : QStringLiteral("uscita"),
                                                                              QString::number(textFadeValue_, 'f', 2)),
                               this);
        }
        update();
        return;
    }
    if (transTrimId_ != 0) {
        if (!transTrimming_ && std::abs(x - pressX_) < 2)
            return;
        const auto it = std::find_if(transitions_.begin(), transitions_.end(),
                                     [this](const TransitionBlock& t) { return t.id == transTrimId_; });
        if (it == transitions_.end())
            return;
        // Il bordo segue il mouse: la durata è il doppio della distanza dalla giunzione, entro i limiti.
        const double desired = 2.0 * std::abs(timeForX(x) - it->junction);
        transTrimming_ = true;
        transTrimDuration_ = transitionLimit ? transitionLimit(transTrimId_, desired) : desired;
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("Durata %1 s").arg(QString::number(transTrimDuration_, 'f', 2)), this);
        invalidate();
        return;
    }
    if (textTrimClip_ != 0) {
        if (!textTrimming_ && std::abs(x - pressX_) < 2)
            return;
        textTrimming_ = true;
        textTrimValue_ = textTrimTarget(x);
        if (const TextBlock* c = findTextClip(textTrimClip_)) {
            const double before = textTrimAtStart_ ? c->position : c->position + c->length;
            const double delta = textTrimValue_ - before;
            QToolTip::showText(event->globalPosition().toPoint(),
                               QStringLiteral("%1 %2 (%3%4 s)")
                                   .arg(textTrimAtStart_ ? QStringLiteral("Inizio") : QStringLiteral("Fine"),
                                        QString::fromStdString(core::formatDuration(textTrimValue_)),
                                        delta >= 0 ? QStringLiteral("+") : QString(), QString::number(delta, 'f', 3)),
                               this);
        }
        update();
        return;
    }
    if (textDragClip_ != 0) {
        if (!textDragging_ && std::abs(x - pressX_) <= 8 && std::abs(pos.y() - pressY_) <= 8)
            return;
        if (!textDragging_) {
            textDragging_ = true;
            setCursor(Qt::ClosedHandCursor);
        }
        // In verticale si cambia livello di scritte.
        if (const int lane = textLaneAt(pos.y()); lane >= 0)
            textDragLane_ = lane;
        textDragStart_ = textDragTarget(x, textDragLane_);
        invalidate();
        return;
    }
    if (trimIndex_ >= 0) {
        if (!trimming_ && std::abs(x - pressX_) < 2)
            return;
        trimming_ = true;
        trimRipple_ = (event->modifiers() & Qt::AltModifier) != 0;
        const double value = trimTarget(x, trimRipple_);
        const Block& b = blocks_[static_cast<std::size_t>(trimIndex_)];
        const double before = trimAtStart_ ? b.sourceStart : b.sourceStart + b.length;
        const bool changed = std::abs(value - trimValue_) > 1e-9;
        trimValue_ = value;
        const double delta = value - before;
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1 %2 (%3%4 s)%5")
                               .arg(trimAtStart_ ? QStringLiteral("Inizio") : QStringLiteral("Fine"),
                                    QString::fromStdString(core::formatDuration(value)),
                                    delta >= 0 ? QStringLiteral("+") : QString(),
                                    QString::number(delta, 'f', 3),
                                    trimRipple_ ? QStringLiteral("\nAlt: i pezzi dopo scorrono")
                                                : QStringLiteral("\nCon Alt i pezzi dopo scorrono")),
                           this);
        update();
        if (changed && onTrimPreview)
            onTrimPreview(b.fragmentId, trimAtStart_, value);
        return;
    }
    if (scrubbing_) {
        playhead_ = std::clamp(timeForX(x), 0.0, total_);
        update();
        if (onSeek)
            onSeek(playhead_);
        return;
    }
    if (selecting_) {
        const double t = std::max(0.0, timeForX(x));
        setAudioSelection(std::min(selectionAnchor_, t), std::max(selectionAnchor_, t), selectionTrack_);
        return;
    }
    if (dragIndex_ < 0)
        return;
    if (!dragging_ && std::abs(x - pressX_) > 8) {
        dragging_ = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (dragging_) {
        dragStart_ = dragTarget(x);
        invalidate();
    }
}

void SequenceTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (scrubbing_) {
        scrubbing_ = false;
        return;
    }
    if (transTrimId_ != 0) {
        const int id = transTrimId_;
        const bool trimmed = transTrimming_;
        const double duration = transTrimDuration_;
        transTrimId_ = 0;
        transTrimming_ = false;
        QToolTip::hideText();
        invalidate();
        if (trimmed && onTransitionDuration)
            onTransitionDuration(id, duration);
        return;
    }
    if (jointActive_) {
        const JunctionDot dot = joint_;
        const double db = jointDb_;
        const bool changed = jointMoved_ || !dot.level;
        jointActive_ = false;
        jointMoved_ = false;
        jointFirstLine_.clear();
        jointSecondLine_.clear();
        QToolTip::hideText();
        invalidate();
        if (changed && onJunctionLevel)
            onJunctionLevel(dot.first.fragment, dot.first.id, dot.second.id, db);
        return;
    }
    if (envActive_) {
        const EnvTarget target = envTarget_;
        const std::vector<core::LevelPoint> points = envPoints_;
        const bool changed = envMoved_;
        envActive_ = false;
        envPoint_ = -1;
        envMoved_ = false;
        envPoints_.clear();
        QToolTip::hideText();
        invalidate();
        if (changed && onEnvelopeChanged)
            onEnvelopeChanged(target.fragment, target.id, points);
        return;
    }
    if (levelLane_ >= 0) {
        const int track = lanes_[static_cast<std::size_t>(levelLane_)].trackId;
        const double db = levelValue_;
        levelLane_ = -1;
        QToolTip::hideText();
        invalidate();
        if (onTrackLevel)
            onTrackLevel(track, db);
        return;
    }
    if (audioTrimClip_ != 0) {
        const int clip = audioTrimClip_;
        const bool trimmed = audioTrimming_;
        audioTrimClip_ = 0;
        audioTrimming_ = false;
        QToolTip::hideText();
        update();
        if (trimmed && onAudioTrim)
            onAudioTrim(clip, audioTrimAtStart_, audioTrimValue_, audioTrimRipple_);
        return;
    }
    if (audioDragClip_ != 0) {
        const int clip = audioDragClip_;
        const bool moved = audioDragging_;
        int fromLane = -1;
        const AudioBlock* c = findAudioClip(clip, &fromLane);
        const double from = c != nullptr ? c->position : 0.0;
        const int lane = audioDragLane_;
        const double to = moved ? audioDragTarget(event->position().x(), lane) : from;
        audioDragClip_ = 0;
        audioDragging_ = false;
        setCursor(Qt::ArrowCursor);
        invalidate();
        if (moved && c != nullptr && lane >= 0 && (lane != fromLane || std::abs(to - from) > 1e-6) && onAudioClipMoveTo)
            onAudioClipMoveTo(clip, lanes_[static_cast<std::size_t>(lane)].trackId, to);
        return;
    }
    if (textFadeClip_ != 0) {
        const int clip = textFadeClip_;
        const bool changed = textFading_;
        double fadeIn = 0.0;
        double fadeOut = 0.0;
        if (const TextBlock* c = findTextClip(clip))
            textFadesOf(*c, fadeIn, fadeOut);
        textFadeClip_ = 0;
        textFading_ = false;
        QToolTip::hideText();
        update();
        if (changed && onTextFades)
            onTextFades(clip, fadeIn, fadeOut);
        return;
    }
    if (textTrimClip_ != 0) {
        const int clip = textTrimClip_;
        const bool trimmed = textTrimming_;
        textTrimClip_ = 0;
        textTrimming_ = false;
        QToolTip::hideText();
        update();
        if (trimmed && onTextTrim)
            onTextTrim(clip, textTrimAtStart_, textTrimValue_);
        return;
    }
    if (textDragClip_ != 0) {
        const int clip = textDragClip_;
        const bool moved = textDragging_;
        int fromLane = -1;
        const TextBlock* c = findTextClip(clip, &fromLane);
        const double from = c != nullptr ? c->position : 0.0;
        const int lane = textDragLane_;
        const double to = moved ? textDragTarget(event->position().x(), lane) : from;
        textDragClip_ = 0;
        textDragging_ = false;
        setCursor(Qt::ArrowCursor);
        invalidate();
        if (moved && c != nullptr && lane >= 0 && (lane != fromLane || std::abs(to - from) > 1e-6) && onTextClipMoveTo)
            onTextClipMoveTo(clip, textLanes_[static_cast<std::size_t>(lane)].layerId, to);
        return;
    }
    if (selecting_) {
        selecting_ = false;
        // Un clic senza trascinare toglie la selezione.
        if (std::abs(event->position().x() - pressX_) < 4)
            setAudioSelection(0.0, 0.0, selectionTrack_);
        if (onAudioSelection)
            onAudioSelection(selectionFrom_, selectionTo_);
        return;
    }
    if (trimIndex_ >= 0) {
        const int fragment = blocks_[static_cast<std::size_t>(trimIndex_)].fragmentId;
        const bool trimmed = trimming_;
        trimIndex_ = -1;
        trimming_ = false;
        QToolTip::hideText();
        update();
        if (trimmed && onTrim)
            onTrim(fragment, trimAtStart_, trimValue_, trimRipple_);
        return;
    }
    if (dragging_ && dragIndex_ >= 0) {
        const int fragment = blocks_[dragIndex_].fragmentId;
        const double from = startOfBlock(dragIndex_);
        const double to = dragTarget(event->position().x());
        dragging_ = false;
        dragIndex_ = -1;
        setCursor(Qt::ArrowCursor);
        invalidate();
        if (std::abs(to - from) > 1e-6 && onMoveTo)
            onMoveTo(fragment, to);
        return;
    }
    dragging_ = false;
    dragIndex_ = -1;
}

void SequenceTimeline::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QLatin1String(kTransitionMime))) {
        transDragType_ = QString::fromUtf8(event->mimeData()->data(QLatin1String(kTransitionMime)));
        transDragActive_ = true;
        transDropIndex_ = junctionAt(event->position().x());
        transDropValid_ = transDropIndex_ >= 0
                          && (!canDropTransition || canDropTransition(blocks_[static_cast<std::size_t>(transDropIndex_)].fragmentId));
        event->acceptProposedAction();
        update();
        return;
    }
    if (droppedFiles(event->mimeData()).isEmpty())
        return;
    event->acceptProposedAction();
    fileDropX_ = event->position().x();
    const int lane = laneAt(event->position().y());
    fileDropLane_ = lane > 0 ? lane : -1;
    update();
}

void SequenceTimeline::dragMoveEvent(QDragMoveEvent* event)
{
    if (transDragActive_) {
        const int index = junctionAt(event->position().x());
        if (index != transDropIndex_) {
            transDropIndex_ = index;
            transDropValid_ = index >= 0
                              && (!canDropTransition || canDropTransition(blocks_[static_cast<std::size_t>(index)].fragmentId));
            update();
        }
        event->acceptProposedAction();
        return;
    }
    if (fileDropX_ < 0)
        return;
    event->acceptProposedAction();
    fileDropX_ = event->position().x();
    const int lane = laneAt(event->position().y());
    fileDropLane_ = lane > 0 ? lane : -1;
    update();
}

void SequenceTimeline::dragLeaveEvent(QDragLeaveEvent*)
{
    transDragActive_ = false;
    transDropIndex_ = -1;
    fileDropX_ = -1.0;
    fileDropLane_ = -1;
    update();
}

void SequenceTimeline::dropEvent(QDropEvent* event)
{
    if (transDragActive_) {
        const int index = junctionAt(event->position().x());
        const QString type = transDragType_;
        const bool valid = index >= 0
                           && (!canDropTransition || canDropTransition(blocks_[static_cast<std::size_t>(index)].fragmentId));
        const int fragment = index >= 0 ? blocks_[static_cast<std::size_t>(index)].fragmentId : 0;
        transDragActive_ = false;
        transDropIndex_ = -1;
        update();
        event->acceptProposedAction();
        if (valid && onTransitionDropped)
            onTransitionDropped(type, fragment);
        else if (onTransitionDropRefused)
            onTransitionDropRefused(fragment);
        return;
    }
    const QStringList files = droppedFiles(event->mimeData());
    const int lane = laneAt(event->position().y());
    const int index = blocks_.empty() ? 0 : dropIndexAt(event->position().x());
    const double at = std::max(0.0, timeForX(event->position().x()));
    fileDropX_ = -1.0;
    fileDropLane_ = -1;
    update();
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    if (lane > 0) {
        if (onAudioFilesDropped)
            onAudioFilesDropped(files, lanes_[static_cast<std::size_t>(lane)].trackId, at);
        return;
    }
    if (onFilesDropped)
        onFilesDropped(files, index);
}

void SequenceTimeline::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    {
        bool edge = false;
        bool atStart = false;
        if (const int index = transitionAt(pos, edge, atStart); index >= 0 && pos.x() >= videoLane().left()) {
            if (onTransitionActivate)
                onTransitionActivate(transitions_[static_cast<std::size_t>(index)].id);
            return;
        }
    }
    if (textLaneAt(pos.y()) >= 0) {
        int lane = 0;
        int clip = 0;
        if (textClipAt(pos, lane, clip) && onTextActivate)
            onTextActivate(textLanes_[static_cast<std::size_t>(lane)].clips[static_cast<std::size_t>(clip)].clipId);
        return;
    }
    if (pos.x() < videoLane().left() - 2) {
        // Doppio clic sul livello: torna a 0 dB.
        const int lane = laneAt(pos.y());
        if (lane >= 0 && levelBar(lane).contains(pos) && onTrackLevel)
            onTrackLevel(lanes_[static_cast<std::size_t>(lane)].trackId, 0.0);
        return;
    }
    if (JunctionDot dot; junctionHit(pos, dot)) {
        // Doppio clic sul pallino della giunzione: lo toglie.
        if (dot.level && onJunctionLevel)
            onJunctionLevel(dot.first.fragment, dot.first.id, dot.second.id, std::nullopt);
        return;
    }
    EnvTarget envTarget;
    int envPoint = -1;
    if (envelopeHit(pos, envTarget, envPoint)) {
        // Doppio clic su un punto della linea del livello: lo toglie.
        if (envPoint >= 0 && onEnvelopeChanged) {
            std::vector<core::LevelPoint> points = *envTarget.line;
            points.erase(points.begin() + envPoint);
            onEnvelopeChanged(envTarget.fragment, envTarget.id, points);
        }
        return;
    }
    if (pos.y() >= audioLane(0).top() - kLaneGap / 2)
        return;
    const int index = blockAt(pos.x());
    if (index >= 0 && onActivate)
        onActivate(blocks_[index].fragmentId);
}

void SequenceTimeline::resizeEvent(QResizeEvent* event)
{
    cacheValid_ = false;
    ZoomableTimeline::resizeEvent(event);
    const int size = kRulerHeight - 2;
    addTrack_->setGeometry(6, 1, size + 4, size);
    removeTrack_->setGeometry(6 + size + 6, 1, size + 4, size);
}

}  // namespace editor::app
