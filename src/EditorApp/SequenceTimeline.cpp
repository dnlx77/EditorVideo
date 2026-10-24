#include "EditorApp/SequenceTimeline.h"

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

namespace {

constexpr double kTop = 6;           // spazio fra il righello e la corsia video
constexpr double kVideoHeight = 54;    // corsia video quando c'è spazio; con poco spazio si stringe
constexpr double kLaneGap = 4;
constexpr double kHeaderWidth = 118;  // a sinistra delle corsie: nome, muto, solo, livello
constexpr double kAudioLaneHeight = 40;  // altezza in più che chiede ogni traccia aggiunta

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
                              "Audio: trascina per selezionare un tratto (o I e O nel Montaggio) · su A1 M toglie l'audio, "
                              "Maiusc+M lo rimette · sulle tracce aggiunte trascina le clip (anche su un'altra traccia), "
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
    for (std::size_t i = 1; i < lanes_.size(); ++i) {
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
    for (std::size_t i = 1; i < lanes_.size(); ++i) {
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
    const QRectF solo = soloBox(index);
    const double height = std::clamp(header.height() - 4, 10.0, 16.0);
    return QRectF(QPointF(solo.right() + 5, header.center().y() - height / 2), QPointF(header.right() - 3, header.center().y() + height / 2));
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
    // Aggancio magnetico a meno di 10 pixel: cursore, inizio della sequenza e bordi degli altri blocchi.
    std::vector<double> marks{0.0, playhead_};
    for (int i = 0; i < static_cast<int>(blocks_.size()); ++i) {
        if (i != trimIndex_) {
            marks.push_back(starts_[static_cast<std::size_t>(i)]);
            marks.push_back(starts_[static_cast<std::size_t>(i)] + blocks_[static_cast<std::size_t>(i)].length);
        }
    }
    double best = 10.0 / zoom().pixelsPerSecond();
    double snapped = t;
    for (const double mark : marks) {
        if (std::abs(t - mark) < best) {
            best = std::abs(t - mark);
            snapped = mark;
        }
    }
    t = snapped;
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
    // Aggancio magnetico: l'inizio o la fine del blocco, se a meno di 10 pixel da un bordo di un altro blocco o
    // dall'inizio della sequenza. (Il cursore no: il clic per prendere il blocco lo porta proprio lì.)
    std::vector<double> edges{0.0};
    for (int i = 0; i < static_cast<int>(blocks_.size()); ++i) {
        if (i != dragIndex_) {
            edges.push_back(starts_[static_cast<std::size_t>(i)]);
            edges.push_back(starts_[static_cast<std::size_t>(i)] + blocks_[static_cast<std::size_t>(i)].length);
        }
    }
    const double reach = 10.0 / zoom().pixelsPerSecond();
    double best = reach;
    double snapped = desired;
    for (const double edge : edges) {
        for (const double offset : {0.0, dragged.length}) {
            const double distance = std::abs(desired + offset - edge);
            if (distance < best) {
                best = distance;
                snapped = edge - offset;
            }
        }
    }
    desired = std::max(0.0, snapped);
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

std::vector<double> SequenceTimeline::snapMarks(int ignoredClip) const
{
    std::vector<double> marks{0.0, playhead_};
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        marks.push_back(starts_[i]);
        marks.push_back(starts_[i] + blocks_[i].length);
    }
    for (std::size_t l = 1; l < lanes_.size(); ++l) {
        for (const AudioBlock& c : lanes_[l].clips) {
            if (c.clipId != ignoredClip) {
                marks.push_back(c.position);
                marks.push_back(c.position + c.length);
            }
        }
    }
    return marks;
}

bool SequenceTimeline::audioClipAt(const QPointF& pos, int& lane, int& clip) const
{
    lane = laneAt(pos.y());
    if (lane <= 0 || pos.x() < videoLane().left())
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
    if (lane <= 0 || pos.x() < videoLane().left())
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
    if (c == nullptr || lane <= 0)
        return 0.0;
    double desired = timeForX(x) - audioDragGrab_;
    // Aggancio magnetico dell'inizio o della fine, a meno di 10 pixel.
    double best = 10.0 / zoom().pixelsPerSecond();
    double snapped = desired;
    for (const double mark : snapMarks(c->clipId)) {
        for (const double offset : {0.0, c->length}) {
            const double distance = std::abs(desired + offset - mark);
            if (distance < best) {
                best = distance;
                snapped = mark - offset;
            }
        }
    }
    desired = std::max(0.0, snapped);
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
    double best = 10.0 / zoom().pixelsPerSecond();
    double snapped = t;
    for (const double mark : snapMarks(c->clipId)) {
        if (std::abs(t - mark) < best) {
            best = std::abs(t - mark);
            snapped = mark;
        }
    }
    t = snapped;
    const double desired = (audioTrimAtStart_ ? c->sourceStart : c->sourceStart + c->length) + (t - edge);
    return audioTrimLimit ? audioTrimLimit(c->clipId, audioTrimAtStart_, desired, ripple) : desired;
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
    if (blocks_.empty() && lanes_.size() == 1)
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
    paintRuler(p);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(a, 4, 4);
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i)
        p.drawRoundedRect(audioLane(i), 4, 4);

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
        for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
            if (audioLaneSelected(i))
                frame(audioLane(i));
        }
    }

    // Dove andrà la clip audio trascinata: il suo contorno sulla corsia di arrivo.
    if (audioDragging_ && audioDragLane_ > 0) {
        if (const AudioBlock* c = findAudioClip(audioDragClip_)) {
            const QRectF lane = audioLane(audioDragLane_);
            const QRectF target(QPointF(xForTime(audioDragStart_) + 1, lane.top() + 1),
                                QPointF(xForTime(audioDragStart_ + c->length) - 1, lane.bottom() - 1));
            QColor fill = blockColor(c->colorIndex);
            fill.setAlpha(150);
            p.setBrush(fill);
            p.setPen(QPen(pal.color(QPalette::Highlight), 3));
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
        p.setPen(QPen(pal.color(QPalette::Highlight), 3));
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
        p.setPen(b.fragmentId == selected_ ? QPen(pal.color(QPalette::Highlight), 3) : QPen(color.darker(140), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 3, 3);
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

void SequenceTimeline::paintAudio(QPainter& p)
{
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const QRectF lane = audioLane(i);
        p.save();
        p.setClipRect(lane);
        if (i == 0)
            paintA1(p, lane);
        else
            paintTrack(p, i, lane);
        // Una corsia che non si sente (muto, o non in solo quando altre lo sono): scurita.
        if (!lanes_[static_cast<std::size_t>(i)].audible) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 70));
            p.drawRect(lane);
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        if (i == 0) {
            for (std::size_t b = 0; b < blocks_.size(); ++b)
                paintEnvelope(p, targetOfBlock(b), lane);
        } else {
            for (std::size_t c = 0; c < lanes_[static_cast<std::size_t>(i)].clips.size(); ++c)
                paintEnvelope(p, targetOfClip(i, c), lane);
        }
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
    if (lane == 0) {
        const int block = blockAt(pos.x());
        if (block < 0 || t < starts_[static_cast<std::size_t>(block)] || t > starts_[static_cast<std::size_t>(block)] + blocks_[static_cast<std::size_t>(block)].length)
            return false;
        found = targetOfBlock(static_cast<std::size_t>(block));
    } else {
        const auto& clips = lanes_[static_cast<std::size_t>(lane)].clips;
        bool any = false;
        for (std::size_t i = 0; i < clips.size() && !any; ++i) {
            if (t >= clips[i].position && t <= clips[i].position + clips[i].length) {
                found = targetOfClip(lane, i);
                any = true;
            }
        }
        if (!any)
            return false;
    }
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
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(lane, Qt::AlignCenter, QStringLiteral("%1: trascina qui un file audio").arg(track.name));
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
        p.setPen(c.clipId == selectedAudioClip_ ? QPen(pal.color(QPalette::Highlight), 3) : QPen(color.darker(140), 1));
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

void SequenceTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    pressX_ = pos.x();
    pressY_ = pos.y();
    const double t = timeForX(pressX_);
    const bool content = !blocks_.empty() || lanes_.size() > 1;

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
    if (const int lane = laneAt(pos.y()); lane >= 0) {
        // Corsie audio: la linea del livello, le clip delle tracce aggiunte (bordi o spostamento), altrimenti la
        // selezione di un tratto.
        selected_ = 0;
        selectedGap_ = 0;
        setSelectedTrack(lanes_[static_cast<std::size_t>(lane)].trackId);
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
        selecting_ = true;
        selectionAnchor_ = std::max(0.0, t);
        setAudioSelection(selectionAnchor_, selectionAnchor_, lanes_[static_cast<std::size_t>(lane)].trackId);
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
        if (envelopeHit(pos, envTarget, envPoint))
            setCursor(envPoint >= 0 ? Qt::SizeAllCursor : Qt::SizeVerCursor);
        else if (edgeAt(pos, atStart) >= 0 || audioEdgeAt(pos, lane, clip, atStart) || (header >= 0 && levelBar(header).contains(pos)))
            setCursor(Qt::SizeHorCursor);
        else
            setCursor(Qt::ArrowCursor);
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
        // In verticale si cambia traccia (solo fra quelle aggiunte).
        if (const int lane = laneAt(pos.y()); lane > 0)
            audioDragLane_ = lane;
        audioDragStart_ = audioDragTarget(x, audioDragLane_);
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
        if (moved && c != nullptr && lane > 0 && (lane != fromLane || std::abs(to - from) > 1e-6) && onAudioClipMoveTo)
            onAudioClipMoveTo(clip, lanes_[static_cast<std::size_t>(lane)].trackId, to);
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
    fileDropX_ = -1.0;
    fileDropLane_ = -1;
    update();
}

void SequenceTimeline::dropEvent(QDropEvent* event)
{
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
    if (pos.x() < videoLane().left() - 2) {
        // Doppio clic sul livello: torna a 0 dB.
        const int lane = laneAt(pos.y());
        if (lane >= 0 && levelBar(lane).contains(pos) && onTrackLevel)
            onTrackLevel(lanes_[static_cast<std::size_t>(lane)].trackId, 0.0);
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
