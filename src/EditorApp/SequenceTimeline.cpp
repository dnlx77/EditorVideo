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


QStringList droppedFiles(const QMimeData* mime)
{
    QStringList files;
    if (mime == nullptr || !mime->hasUrls())
        return files;
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile())
            files.append(QDir::toNativeSeparators(url.toLocalFile()));
    }
    return files;
}

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
    setAcceptDrops(true);  // video trascinati da Esplora file
    setToolTip(QStringLiteral("Video: clic per andare lì · trascina un pezzo per spostarlo avanti o indietro (lascia "
                              "spazi vuoti, da riempire con altri pezzi) · doppio clic per riaprirlo a sinistra · "
                              "Canc: togli il pezzo o chiudi lo spazio vuoto selezionato · S: dividi al cursore\n"
                              "Audio: sulle tracce aggiunte trascina lo sfondo per selezionare un tratto · trascina le clip (anche su un'altra traccia), "
                              "i bordi le accorciano, S divide, Canc toglie la clip o il tratto selezionato\n"
                              "Ctrl+rotellina (o + e -): ingrandisci e riduci · rotellina: scorri"));

    // In alto a sinistra: aggiungi e togli una traccia audio.
    addTrack_ = new QToolButton(this);
    addTrack_->setText(QStringLiteral("+"));
    addTrack_->setToolTip(QStringLiteral("Aggiungi una traccia audio"));
    addTrack_->setAutoRaise(true);
    connect(addTrack_, &QToolButton::clicked, this, [this] {
        if (onAddTrack)
            onAddTrack();
    });
    removeTrack_ = new QToolButton(this);
    removeTrack_->setText(QStringLiteral("\u2212"));
    removeTrack_->setAutoRaise(true);
    connect(removeTrack_, &QToolButton::clicked, this, [this] {
        // La traccia scelta (clic sul suo nome o su una sua clip), altrimenti l'ultima.
        int track = lanes_.size() > 1 ? lanes_.back().trackId : 0;
        if (selectedTrack_ > 0 && std::any_of(lanes_.begin() + 1, lanes_.end(), [this](const AudioLane& l) { return l.trackId == selectedTrack_; }))
            track = selectedTrack_;
        if (track > 0 && onRemoveTrack)
            onRemoveTrack(track);
    });
    syncTrackButtons();
}

QSize SequenceTimeline::sizeHint() const
{
    const int extra = static_cast<int>(lanes_.size() - 1) * static_cast<int>(kAudioLaneHeight);
    return {800, 132 + kRulerHeight + kControlsHeight + extra};
}

QSize SequenceTimeline::minimumSizeHint() const
{
    return {100 + static_cast<int>(kHeaderWidth), 48 + kRulerHeight + kControlsHeight + static_cast<int>(lanes_.size() - 1) * 18};
}

void SequenceTimeline::syncTrackButtons()
{
    const bool any = lanes_.size() > 1;
    removeTrack_->setEnabled(any);
    QString name = any ? lanes_.back().name : QString();
    for (std::size_t i = 1; i < lanes_.size(); ++i) {
        if (lanes_[i].trackId == selectedTrack_)
            name = lanes_[i].name;
    }
    removeTrack_->setToolTip(any ? QStringLiteral("Togli la traccia audio %1 (con le sue clip; Ctrl+Z la rimette)").arg(name)
                                 : QStringLiteral("Togli una traccia audio (non ce ne sono: A1 resta sempre)"));
}

void SequenceTimeline::updateDuration()
{
    double end = total_;
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
        for (const AudioBlock& c : lanes_[i].clips)
            end = std::max(end, c.position + c.length);
    }
    // Dopo la fine resta sempre un po' di posto (anche con "Adatta"), per poter trascinare più avanti.
    setTimelineDuration(end <= 0 ? 0.0 : end + std::max(2.0, end * 0.15));
}

void SequenceTimeline::setBlocks(std::vector<Block> blocks)
{
    blocks_ = std::move(blocks);
    total_ = 0.0;
    starts_.clear();
    starts_.reserve(blocks_.size());
    for (const Block& b : blocks_) {
        starts_.push_back(total_ + b.gap);
        total_ += b.gap + b.length;
    }
    if (std::none_of(blocks_.begin(), blocks_.end(), [this](const Block& b) { return b.fragmentId == selected_; }))
        selected_ = 0;
    if (std::none_of(blocks_.begin(), blocks_.end(), [this](const Block& b) { return b.fragmentId == selectedGap_ && b.gap > 0; }))
        selectedGap_ = 0;
    std::erase_if(multi_, [this](const Object& o) {
        return o.kind != Object::Kind::AudioClip
               && std::none_of(blocks_.begin(), blocks_.end(), [&](const Block& b) { return b.fragmentId == o.id; });
    });
    if (multi_.size() < 2)
        multi_.clear();
    updateDuration();
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
    multi_.clear();
    selected_ = fragmentId;
    if (fragmentId != 0) {
        selectedGap_ = 0;
        selectedAudioClip_ = 0;
        selectedTrack_ = -1;  // la traccia scelta è quella video
        syncTrackButtons();
    }
    invalidate();
}

void SequenceTimeline::setSelectedGap(int fragmentId)
{
    if (fragmentId != 0)
        multi_.clear();
    selectedGap_ = fragmentId;
    if (fragmentId != 0) {
        selected_ = 0;
        selectedAudioClip_ = 0;
        selectedTrack_ = -1;
        syncTrackButtons();
    }
    invalidate();
}

void SequenceTimeline::setMuted(std::vector<core::TimeRange> ranges)
{
    muted_ = std::move(ranges);
    invalidate();
}

void SequenceTimeline::setAudioLanes(std::vector<AudioLane> lanes)
{
    if (lanes.empty() || lanes.front().trackId != 0)
        lanes.insert(lanes.begin(), AudioLane{0, QStringLiteral("A1")});
    const bool resized = lanes.size() != lanes_.size();
    lanes_ = std::move(lanes);
    const auto hasTrack = [this](int trackId) {
        return std::any_of(lanes_.begin(), lanes_.end(), [trackId](const AudioLane& l) { return l.trackId == trackId; });
    };
    if (selectedAudioClip_ != 0 && findAudioClip(selectedAudioClip_) == nullptr)
        selectedAudioClip_ = 0;
    std::erase_if(multi_, [this](const Object& o) { return o.kind == Object::Kind::AudioClip && findAudioClip(o.id) == nullptr; });
    if (multi_.size() < 2)
        multi_.clear();
    if (selectedTrack_ >= 0 && !hasTrack(selectedTrack_))
        selectedTrack_ = -1;
    if (!hasTrack(selectionTrack_))
        setAudioSelection(0.0, 0.0);
    if (audioDragClip_ != 0 && findAudioClip(audioDragClip_) == nullptr) {
        audioDragClip_ = 0;
        audioDragging_ = false;
    }
    if (audioTrimClip_ != 0 && findAudioClip(audioTrimClip_) == nullptr) {
        audioTrimClip_ = 0;
        audioTrimming_ = false;
    }
    if (levelLane_ >= static_cast<int>(lanes_.size()))
        levelLane_ = -1;
    syncTrackButtons();
    updateDuration();
    if (resized)
        updateGeometry();  // ogni traccia chiede un po' di altezza in più
    invalidate();
}

void SequenceTimeline::setWaveforms(const std::function<std::shared_ptr<const core::Waveform>(int clipId)>& forClip,
                                    const std::function<std::shared_ptr<const core::Waveform>(const QString& path)>& forPath)
{
    for (Block& b : blocks_)
        b.waveform = forClip(b.clipId);
    for (AudioLane& lane : lanes_) {
        for (AudioBlock& c : lane.clips)
            c.waveform = forPath(c.path);
    }
    invalidate();
}

void SequenceTimeline::setAudioSelection(double from, double to, int trackId)
{
    selectionFrom_ = from;
    selectionTo_ = std::max(from, to);
    selectionTrack_ = trackId;
    if (to > from && trackId >= 0 && std::any_of(lanes_.begin(), lanes_.end(), [trackId](const AudioLane& l) { return l.trackId == trackId; })) {
        selected_ = 0;
        selectedGap_ = 0;
        selectedTrack_ = trackId;  // la traccia del tratto scelto è quella evidenziata
        syncTrackButtons();
    }
    invalidate();
}

void SequenceTimeline::setSelectedAudioClip(int clipId)
{
    multi_.clear();
    selectedAudioClip_ = clipId;
    if (clipId != 0) {
        selected_ = 0;
        selectedGap_ = 0;
        int lane = -1;
        if (findAudioClip(clipId, &lane) != nullptr)
            selectedTrack_ = lanes_[static_cast<std::size_t>(lane)].trackId;
        syncTrackButtons();
    }
    invalidate();
}

std::vector<SequenceTimeline::Object> SequenceTimeline::allSelectedObjects() const
{
    if (!multi_.empty())
        return multi_;
    std::vector<Object> one;
    if (selected_ != 0)
        one.push_back({Object::Kind::Piece, selected_});
    if (selectedAudioClip_ != 0)
        one.push_back({Object::Kind::AudioClip, selectedAudioClip_});
    return one;
}

bool SequenceTimeline::isSelected(Object::Kind kind, int id) const
{
    if (!multi_.empty())
        return std::find(multi_.begin(), multi_.end(), Object{kind, id}) != multi_.end();
    return (kind == Object::Kind::Piece && id == selected_) || (kind == Object::Kind::AudioClip && id == selectedAudioClip_);
}

bool SequenceTimeline::isMate(Object::Kind kind, int id) const
{
    const bool member = std::find(mates_.begin(), mates_.end(), Object{kind, id}) != mates_.end();
    return member && (matesGrouped_ || !isSelected(kind, id));
}

bool SequenceTimeline::ctrlPress(const QPointF& pos)
{
    // L'oggetto sotto il mouse: un pezzo sulla corsia video, sulle corsie audio una clip o (su A1) l'audio di un pezzo.
    const double t = timeForX(pos.x());
    const auto pieceAt = [&]() {
        const int index = blockAt(pos.x());
        if (index < 0 || t < starts_[static_cast<std::size_t>(index)] || t >= starts_[static_cast<std::size_t>(index)] + blocks_[static_cast<std::size_t>(index)].length)
            return 0;
        return blocks_[static_cast<std::size_t>(index)].fragmentId;
    };
    Object target;
    if (const int lane = laneAt(pos.y()); lane >= 0) {
        int clipLane = 0;
        int clip = 0;
        if (audioClipAt(pos, clipLane, clip)) {
            target = {Object::Kind::AudioClip, lanes_[static_cast<std::size_t>(clipLane)].clips[static_cast<std::size_t>(clip)].clipId};
        } else if (const int piece = lane == 0 ? pieceAt() : 0; piece != 0 && !blocks_[static_cast<std::size_t>(blockAt(pos.x()))].audioDetached) {
            target = {Object::Kind::PieceAudio, piece};
        } else {
            return false;
        }
    } else if (!blocks_.empty() && pos.y() <= videoLane().bottom() + kLaneGap / 2) {
        const int piece = pieceAt();
        if (piece == 0)
            return false;
        target = {Object::Kind::Piece, piece};
    } else {
        return false;
    }
    std::vector<Object> set = allSelectedObjects();
    if (const auto at = std::find(set.begin(), set.end(), target); at != set.end())
        set.erase(at);
    else
        set.push_back(target);
    // Il "principale" (quello che usano i comandi che lavorano su un solo oggetto) è l'ultimo toccato, o l'ultimo rimasto.
    const Object primary = std::find(set.begin(), set.end(), target) != set.end() ? target : (set.empty() ? Object{} : set.back());
    multi_ = set.size() >= 2 ? set : std::vector<Object>{};
    selected_ = primary.kind == Object::Kind::Piece ? primary.id : 0;
    selectedAudioClip_ = primary.kind == Object::Kind::AudioClip ? primary.id : 0;
    selectedGap_ = 0;
    selectedTrack_ = -1;
    syncTrackButtons();
    if (onSelect)
        onSelect(selected_);
    if (onAudioClipSelected)
        onAudioClipSelected(selectedAudioClip_);
    if (onMultiSelection)
        onMultiSelection();
    invalidate();
    return true;
}

void SequenceTimeline::setSelectedTrack(int trackId)
{
    selectedTrack_ = trackId;
    if (trackId >= 0) {
        selected_ = 0;  // una sola traccia è quella scelta: o video o audio
        selectedGap_ = 0;
    }
    syncTrackButtons();
    invalidate();
}

const SequenceTimeline::AudioBlock* SequenceTimeline::findAudioClip(int clipId, int* lane) const
{
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
        for (const AudioBlock& c : lanes_[i].clips) {
            if (c.clipId == clipId) {
                if (lane != nullptr)
                    *lane = static_cast<int>(i);
                return &c;
            }
        }
    }
    return nullptr;
}

double SequenceTimeline::videoHeight() const
{
    // La corsia video fino a kVideoHeight; le corsie audio si dividono il resto.
    const double n = static_cast<double>(lanes_.size());
    const double lanes = height() - kRulerHeight - kTop - 6 - kControlsHeight - n * kLaneGap;
    return std::clamp(lanes / (n + 1) * 1.2, 18.0, kVideoHeight);
}

bool SequenceTimeline::videoLaneSelected() const
{
    return selected_ != 0 || selectedGap_ != 0;
}

bool SequenceTimeline::audioLaneSelected(int index) const
{
    return selectedTrack_ >= 0 && lanes_[static_cast<std::size_t>(index)].trackId == selectedTrack_;
}

QRectF SequenceTimeline::videoLane() const
{
    return QRectF(6 + kHeaderWidth, kRulerHeight + kTop, std::max(1.0, width() - 12 - kHeaderWidth), videoHeight());
}

QRectF SequenceTimeline::audioLane(int index) const
{
    const QRectF video = videoLane();
    const double n = static_cast<double>(lanes_.size());
    const double top = video.bottom() + kLaneGap;
    const double available = height() - top - 6 - kControlsHeight - (n - 1) * kLaneGap;
    const double laneHeight = std::max(14.0, available / n);
    return QRectF(video.left(), top + index * (laneHeight + kLaneGap), video.width(), laneHeight);
}

int SequenceTimeline::laneAt(double y) const
{
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const QRectF lane = audioLane(i);
        if (y >= lane.top() - kLaneGap / 2 && y < lane.bottom() + kLaneGap / 2)
            return i;
    }
    return -1;
}

QRectF SequenceTimeline::headerRect(const QRectF& lane) const
{
    return QRectF(6, lane.top(), kHeaderWidth - 8, lane.height());
}

QRectF SequenceTimeline::muteBox(int index) const
{
    const QRectF header = headerRect(audioLane(index));
    const double size = std::clamp(header.height() - 4, 10.0, 16.0);
    return QRectF(header.left() + 28, header.center().y() - size / 2, size, size);
}

QRectF SequenceTimeline::soloBox(int index) const
{
    const QRectF mute = muteBox(index);
    return mute.translated(mute.width() + 3, 0);
}

QRectF SequenceTimeline::levelBar(int index) const
{
    const QRectF header = headerRect(audioLane(index));
    const QRectF last = soloBox(index);
    const double height = std::clamp(header.height() - 4, 10.0, 16.0);
    return QRectF(QPointF(last.right() + 5, header.center().y() - height / 2), QPointF(header.right() - 3, header.center().y() + height / 2));
}

double SequenceTimeline::levelForX(int index, double x) const
{
    const QRectF bar = levelBar(index);
    const double fraction = std::clamp((x - bar.left()) / std::max(1.0, bar.width()), 0.0, 1.0);
    double db = core::AudioTracks::kMinLevelDb + fraction * (core::AudioTracks::kMaxLevelDb - core::AudioTracks::kMinLevelDb);
    db = std::round(db * 2) / 2;  // mezzi dB
    return std::abs(db) < 1.0 ? 0.0 : db;  // vicino a 0 dB si aggancia lì
}

QRectF SequenceTimeline::visiblePart(const QRectF& r) const
{
    return QRectF(QPointF(std::max(r.left(), visibleLeft() - 4), r.top()), QPointF(std::min(r.right(), visibleRight() + 4), r.bottom()));
}

std::vector<QRectF> SequenceTimeline::blockRects(const QRectF& lane) const
{
    std::vector<QRectF> rects;
    rects.reserve(blocks_.size());
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const double start = starts_[i];
        rects.emplace_back(QPointF(xForTime(start) + 1, lane.top() + 2),
                           QPointF(xForTime(start + blocks_[i].length) - 1, lane.bottom() - 2));
    }
    return rects;
}

double SequenceTimeline::xForBlockEdge(int index, bool atStart) const
{
    if (index < 0 || index >= static_cast<int>(blocks_.size()))
        return 0.0;
    const double start = starts_[static_cast<std::size_t>(index)];
    return xForTime(atStart ? start : start + blocks_[static_cast<std::size_t>(index)].length);
}

int SequenceTimeline::edgeAt(const QPointF& pos, bool& atStart) const
{
    const QRectF lane = videoLane();
    if (pos.y() < lane.top() || pos.y() > lane.bottom())
        return -1;
    constexpr double kReach = 5.0;  // pixel
    int best = -1;
    double bestDistance = kReach + 1;
    for (int i = 0; i < static_cast<int>(blocks_.size()); ++i) {
        const double left = xForBlockEdge(i, true);
        const double right = xForBlockEdge(i, false);
        if (right - left < 12)
            continue;  // blocco troppo stretto: si prende per spostarlo
        // Dove due blocchi si toccano vince il lato del bordo su cui sta il mouse.
        for (const bool start : {true, false}) {
            const double edge = start ? left : right;
            const double distance = std::abs(pos.x() - edge);
            const bool inside = start ? pos.x() >= edge : pos.x() <= edge;
            if (distance <= kReach && (distance < bestDistance || (distance == bestDistance && inside))) {
                best = i;
                bestDistance = distance;
                atStart = start;
            }
        }
    }
    return best;
}

double SequenceTimeline::trimTarget(double x, bool ripple) const
{
    if (trimIndex_ < 0 || trimIndex_ >= static_cast<int>(blocks_.size()))
        return 0.0;
    const Block& b = blocks_[static_cast<std::size_t>(trimIndex_)];
    const double start = starts_[static_cast<std::size_t>(trimIndex_)];
    const double edge = trimAtStart_ ? start : start + b.length;
    double t = timeForX(x) - trimGrab_;
    t = snapped(t, snapMarks(trimIndex_, 0, true, false), {0.0});
    const double desired = (trimAtStart_ ? b.sourceStart : b.sourceStart + b.length) + (t - edge);
    return trimLimit ? trimLimit(b.fragmentId, trimAtStart_, desired, ripple) : desired;
}

double SequenceTimeline::trimEdgeSequenceTime(double value) const
{
    const Block& b = blocks_[static_cast<std::size_t>(trimIndex_)];
    const double start = starts_[static_cast<std::size_t>(trimIndex_)];
    // Come se il bordo si spostasse nella sequenza della stessa quantità che nel file.
    return trimAtStart_ ? start + (value - b.sourceStart) : start + b.length + (value - b.sourceStart - b.length);
}

int SequenceTimeline::blockAt(double x) const
{
    const double t = timeForX(x);
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (t < starts_[i])
            return -1;  // spazio vuoto
        if (t < starts_[i] + blocks_[i].length)
            return static_cast<int>(i);
    }
    return blocks_.empty() ? -1 : static_cast<int>(blocks_.size()) - 1;
}

int SequenceTimeline::gapAt(double x) const
{
    const double t = timeForX(x);
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].gap > 0 && t >= starts_[i] - blocks_[i].gap && t < starts_[i])
            return static_cast<int>(i);
    }
    return -1;
}

double SequenceTimeline::startOfBlock(int index) const
{
    if (index < 0 || index >= static_cast<int>(starts_.size()))
        return starts_.empty() ? 0.0 : starts_.back() + blocks_.back().length;
    return starts_[static_cast<std::size_t>(index)];
}

int SequenceTimeline::dropIndexAt(double x) const
{
    // I file vanno prima del primo blocco la cui metà sta a destra del punto (in uno spazio vuoto: prima del blocco
    // che lo segue, cioè all'inizio dello spazio).
    const double t = timeForX(x);
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (t < starts_[i] + blocks_[i].length / 2)
            return static_cast<int>(i);
    }
    return static_cast<int>(blocks_.size());
}

double SequenceTimeline::dragTarget(double x) const
{
    if (dragIndex_ < 0 || dragIndex_ >= static_cast<int>(blocks_.size()))
        return 0.0;
    const Block& dragged = blocks_[dragIndex_];
    double desired = timeForX(x) - dragGrab_;
    // Aggancio ai bordi: l'inizio o la fine del blocco, a meno di 10 pixel da un bordo di un altro blocco o dall'inizio
    // della sequenza. (Il cursore no: il clic per prendere il blocco lo porta proprio lì.)
    desired = std::max(0.0, snapped(desired, snapMarks(dragIndex_, 0, false, false), {0.0, dragged.length}));
    return freeStart ? freeStart(dragged.fragmentId, desired) : desired;
}

bool SequenceTimeline::event(QEvent* event)
{
    // Sul blocco di un pezzo con un segno, su una clip audio o sui pulsanti di una corsia: la spiegazione; altrove il
    // suggerimento generale.
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QPointF pos = help->pos();
        const auto show = [&](const QString& text) {
            QToolTip::showText(help->globalPos(), text, this);
            return true;
        };
        const QRectF lane = videoLane();
        const int index = lane.contains(pos) ? blockAt(pos.x()) : -1;
        if (index >= 0) {
            const std::vector<QRectF> rects = blockRects(lane);
            const Block& b = blocks_[static_cast<std::size_t>(index)];
            QString note = b.fitNote;
            if (b.proxyDivisor > 1)
                note = QStringLiteral("Anteprima dal proxy 1/%1 (l'esportazione usa l'originale)").arg(b.proxyDivisor)
                     + (note.isEmpty() ? QString() : QStringLiteral("\n") + note);
            if (!note.isEmpty() && rects[static_cast<std::size_t>(index)].contains(pos))
                return show(QStringLiteral("%1\n%2").arg(b.label, note));
        }
        if (const int l = laneAt(pos.y()); l >= 0) {
            const AudioLane& a = lanes_[static_cast<std::size_t>(l)];
            if (muteBox(l).contains(pos))
                return show(QStringLiteral("%1: muto (la traccia non si sente)").arg(a.name));
            if (soloBox(l).contains(pos))
                return show(QStringLiteral("%1: solo (se una traccia è in solo si sentono solo quelle in solo)").arg(a.name));
            if (levelBar(l).contains(pos))
                return show(QStringLiteral("%1: livello %2 dB · trascina per cambiarlo, doppio clic per tornare a 0 dB")
                                .arg(a.name, QString::number(a.levelDb, 'f', 1)));
            if (pos.x() < videoLane().left())
                return show(l == 0 ? QStringLiteral("A1: l'audio dei pezzi video")
                                   : QStringLiteral("%1: traccia audio aggiunta · clic per sceglierla (il pulsante − toglie "
                                                    "quella scelta)").arg(a.name));
            int laneIndex = 0;
            int clip = 0;
            if (audioClipAt(pos, laneIndex, clip)) {
                const AudioBlock& c = lanes_[static_cast<std::size_t>(laneIndex)].clips[static_cast<std::size_t>(clip)];
                return show(QStringLiteral("%1\nNel montaggio da %2 a %3 · nel file da %4 a %5")
                                .arg(c.label, QString::fromStdString(core::formatDuration(c.position)),
                                     QString::fromStdString(core::formatDuration(c.position + c.length)),
                                     QString::fromStdString(core::formatDuration(c.sourceStart)),
                                     QString::fromStdString(core::formatDuration(c.sourceStart + c.length))));
            }
        }
    }
    return ZoomableTimeline::event(event);
}

std::vector<double> SequenceTimeline::snapMarks(int ignoredBlock, int ignoredClip, bool withCursor, bool withAudio) const
{
    std::vector<double> marks{0.0};
    if (withCursor)
        marks.push_back(playhead_);
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (static_cast<int>(i) == ignoredBlock)
            continue;
        marks.push_back(starts_[i]);
        marks.push_back(starts_[i] + blocks_[i].length);
    }
    if (withAudio) {
        for (const AudioLane& lane : lanes_) {
            for (const AudioBlock& c : lane.clips) {
                if (c.clipId != ignoredClip) {
                    marks.push_back(c.position);
                    marks.push_back(c.position + c.length);
                }
            }
        }
    }
    return marks;
}

double SequenceTimeline::snapped(double desired, const std::vector<double>& marks, std::initializer_list<double> offsets) const
{
    double best = 10.0 / zoom().pixelsPerSecond();
    double result = desired;
    for (const double mark : marks) {
        for (const double offset : offsets) {
            const double distance = std::abs(desired + offset - mark);
            if (distance < best) {
                best = distance;
                result = mark - offset;
            }
        }
    }
    return result;
}

bool SequenceTimeline::audioClipAt(const QPointF& pos, int& lane, int& clip) const
{
    lane = laneAt(pos.y());
    if (lane < 0 || pos.x() < videoLane().left())
        return false;
    const double t = timeForX(pos.x());
    const auto& clips = lanes_[static_cast<std::size_t>(lane)].clips;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        if (t >= clips[i].position && t < clips[i].position + clips[i].length) {
            clip = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

bool SequenceTimeline::audioEdgeAt(const QPointF& pos, int& lane, int& clip, bool& atStart) const
{
    lane = laneAt(pos.y());
    if (lane < 0 || pos.x() < videoLane().left())
        return false;
    constexpr double kReach = 5.0;  // pixel
    double bestDistance = kReach + 1;
    bool found = false;
    const auto& clips = lanes_[static_cast<std::size_t>(lane)].clips;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const double left = xForTime(clips[i].position);
        const double right = xForTime(clips[i].position + clips[i].length);
        if (right - left < 12)
            continue;  // clip troppo stretta: si prende per spostarla
        for (const bool start : {true, false}) {
            const double edge = start ? left : right;
            const double distance = std::abs(pos.x() - edge);
            const bool inside = start ? pos.x() >= edge : pos.x() <= edge;
            if (distance <= kReach && (distance < bestDistance || (distance == bestDistance && inside))) {
                clip = static_cast<int>(i);
                atStart = start;
                bestDistance = distance;
                found = true;
            }
        }
    }
    return found;
}

double SequenceTimeline::audioDragTarget(double x, int lane) const
{
    const AudioBlock* c = findAudioClip(audioDragClip_);
    if (c == nullptr || lane < 0)
        return 0.0;
    double desired = timeForX(x) - audioDragGrab_;
    // Aggancio ai bordi dell'inizio o della fine, a meno di 10 pixel.
    desired = std::max(0.0, snapped(desired, snapMarks(-1, c->clipId, true, true), {0.0, c->length}));
    const int track = lanes_[static_cast<std::size_t>(lane)].trackId;
    return audioFreePosition ? audioFreePosition(c->clipId, track, desired) : desired;
}

double SequenceTimeline::audioTrimTarget(double x, bool ripple) const
{
    const AudioBlock* c = findAudioClip(audioTrimClip_);
    if (c == nullptr)
        return 0.0;
    const double edge = audioTrimAtStart_ ? c->position : c->position + c->length;
    double t = timeForX(x) - audioTrimGrab_;
    t = snapped(t, snapMarks(-1, c->clipId, true, true), {0.0});
    const double desired = (audioTrimAtStart_ ? c->sourceStart : c->sourceStart + c->length) + (t - edge);
    return audioTrimLimit ? audioTrimLimit(c->clipId, audioTrimAtStart_, desired, ripple) : desired;
}

}  // namespace editor::app
