#include "EditorApp/ProgramMonitor.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/TransportBar.h"
#include "EditorApp/UiText.h"
#include "EditorCore/AudioMixer.h"
#include "EditorCore/Playback.h"
#include "EditorCore/Project.h"
#include "EditorCore/Transitions.h"

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <QtCore/QSignalBlocker>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QPolygonF>

#include <algorithm>
#include <cmath>

namespace editor::app {

ProgramMonitor::ProgramMonitor(const core::Project& project, QWidget* parent)
    : QWidget(parent)
    , project_(project)
{
    player_ = new PlayerWidget;
    updateOverlayFrame();
    player_->setImageMouse([this](QPointF at) { return grabImage(at); }, [this](QPointF at) { dragImage(at); },
                           [this](QPointF at) { dropImage(at); });
    player_->setChromeVisible(false);
    player_->setAudioEnabled(false);  // l'audio del montaggio lo suona il mixer della sequenza (SequenceAudio)
    player_->onVideoDoubleClicked = [this] {
        if (onVideoDoubleClicked)
            onVideoDoubleClicked();
    };
    player_->onWindowEnd = [this] { fragmentEnded(); };

    transport_ = new TransportBar(TransportBar::Kind::Program);
    // Ogni pulsante rende attivo il Montaggio (per i tasti), poi fa il suo comando.
    const auto command = [this](auto action) {
        return [this, action] {
            if (onActivated)
                onActivated();
            action();
        };
    };
    transport_->onStart = command([this] { seekTo(0.0); });
    transport_->onPreviousMark = command([this] { stepToCut(-1); });
    transport_->onPreviousFrame = command([this] { stepFrame(-1); });
    transport_->onPlayPause = command([this] { togglePlayPause(); });
    transport_->onStop = command([this] { stop(); });
    transport_->onNextFrame = command([this] { stepFrame(+1); });
    transport_->onNextMark = command([this] { stepToCut(+1); });
    transport_->onEnd = command([this] { seekToEnd(); });
    transport_->onSpeedChosen = [this](double speed) { setBaseSpeed(speed); };
    transport_->onLoopToggled = [this](bool loop) { setLoop(loop); };
    transport_->onVolumeChanged = [this](int percent) { setVolume(percent); };
    transport_->onMuteToggled = [this](bool muted) { setMuted(muted); };
    transport_->onQualityChosen = [this](int divisor) {
        if (onPreviewQualityChosen)
            onPreviewQualityChosen(divisor);
    };
    timeLabel_ = new QLabel;
    fragmentLabel_ = new QLabel;
    // Le scritte non impongono una larghezza minima: in un pannello stretto si tagliano.
    timeLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fragmentLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    transport_->setInfoWidget(timeLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setSizeConstraint(QLayout::SetNoConstraint);  // la misura minima la decide minimumSizeHint
    layout->addWidget(player_, 1);
    layout->addWidget(transport_);
    // Sotto i comandi: il nome del pezzo e il pulsante dei manipolatori.
    transformButton_ = new QToolButton;
    transformButton_->setText(QStringLiteral("Trasforma"));
    transformButton_->setObjectName(QStringLiteral("pulsanteTrasforma"));
    transformButton_->setCheckable(true);
    transformButton_->setAutoRaise(true);
    transformButton_->setToolTip(QStringLiteral("Manipolatori sul monitor (Q): trascina il pezzo per spostarlo, gli angoli per "
                                                "scalarlo (Maiusc = proporzioni libere, Alt = dal centro), il cerchio sopra "
                                                "per ruotarlo (Maiusc = scatti di 15°). A monitor fermo."));
    connect(transformButton_, &QToolButton::toggled, this, [this](bool on) {
        if (on == transformMode_)
            return;
        setTransformMode(on);
        if (onTransformModeChanged)
            onTransformModeChanged(on);
    });
    cropButton_ = new QToolButton;
    cropButton_->setText(QStringLiteral("Ritaglia"));
    cropButton_->setObjectName(QStringLiteral("pulsanteRitaglia"));
    cropButton_->setCheckable(true);
    cropButton_->setAutoRaise(true);
    cropButton_->setToolTip(QStringLiteral("Ritaglio sul monitor (W): trascina un lato o un angolo del riquadro. A monitor fermo."));
    connect(cropButton_, &QToolButton::toggled, this, [this](bool on) {
        if (on == cropMode_)
            return;
        setCropMode(on);
        if (onCropModeChanged)
            onCropModeChanged(on);
    });
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 4, 0);
    bottom->addWidget(fragmentLabel_, 1);
    bottom->addWidget(transformButton_);
    bottom->addWidget(cropButton_);
    layout->addLayout(bottom);

    poll_ = new QTimer(this);
    poll_->setInterval(40);
    connect(poll_, &QTimer::timeout, this, [this] { updatePosition(); });
    transitionTimer_ = new QTimer(this);
    transitionTimer_->setInterval(5);  // come il lettore
    connect(transitionTimer_, &QTimer::timeout, this, [this] { transitionTick(); });

    updateLabels();
}

ProgramMonitor::~ProgramMonitor()
{
    // Il lettore (figlio di questo widget) sopravvive ai membri: non deve più chiedere l'orologio all'audio.
    player_->setExternalClock(nullptr, 0.0);
    sequenceAudio_.stop();
}

void ProgramMonitor::mousePressEvent(QMouseEvent* event)
{
    if (onActivated)
        onActivated();
    QWidget::mousePressEvent(event);
}

void ProgramMonitor::setVideoSource(std::function<QString(const QString& path)> source)
{
    // Anche la transizione legge i file da qui (i proxy).
    transitionPlayback_.setVideoSource([source](const std::string& path) {
        return source ? source(QString::fromStdString(path)).toStdString() : path;
    });
    player_->videoSourceFor = std::move(source);
}

void ProgramMonitor::reloadVideo()
{
    player_->reloadVideo();
    layerPlaybacks_.clear();  // possono leggere da un altro file (proxy pronto, qualità cambiata)
    if (!playing_)
        updateLayers();
    transitionPlayback_.reset();
    if (transitionId_ != 0 && !playing_) {
        if (const auto placement = project_.transitionPlacement(transitionId_))
            showTransitionFrame(*placement, position_, true);
    }
}

void ProgramMonitor::setPreviewQuality(int divisor)
{
    transport_->setQuality(divisor);
}

void ProgramMonitor::load(int index, double offset)
{
    const auto& fragments = project_.sequence().fragments();
    if (index < 0 || index >= static_cast<int>(fragments.size())) {
        index_ = -1;
        return;
    }
    const core::Fragment& f = fragments[index];
    const core::Clip* clip = project_.find(f.clipId);
    if (clip == nullptr)
        return;
    gapIndex_ = -1;
    transitionId_ = 0;
    primedTransitionId_ = 0;
    transitionFrame_.reset();
    player_->setBlank(false);
    player_->setTransformCanvas(transformMode_ || cropMode_ || layersShown_);
    // Scala, posizione, rotazione... del pezzo (con i fotogrammi chiave, quelli del punto da cui si parte).
    player_->setFrameTransform(core::evaluateTransform(f.transform, f.animation, f.start + std::clamp(offset, 0.0, f.length())), frameSize());
    if (f.clipId != loadedClipId_) {
        player_->openFile(QString::fromStdString(clip->path));
        loadedClipId_ = f.clipId;
    }
    index_ = index;
    // L'orologio è quello dell'audio del montaggio (che non si ferma ai cambi di pezzo), se sta suonando: al tempo t
    // del file corrisponde il tempo t + (inizio del pezzo - inizio nel file) della sequenza.
    player_->setExternalClock(
        [this]() -> std::optional<double> {
            if (!sequenceAudio_.running())
                return std::nullopt;
            return sequenceAudio_.position();
        },
        project_.sequence().startOf(index) - f.start);
    player_->setPlaybackWindow(f.start, f.end);
    player_->seekTo(f.start + std::clamp(offset, 0.0, f.length()));
}

std::vector<core::TextClip> ProgramMonitor::visibleText() const
{
    std::vector<core::TextClip> clips;
    if (showingSource_)
        return clips;
    for (const core::TextClip* clip : project_.textTracks().visibleAt(position_))
        clips.push_back(*clip);
    return clips;
}

// ---- Manipolatori

const core::LayerPiece* ProgramMonitor::manipPiece() const
{
    // Il pezzo di un livello (V2, V3...) scelto sulla linea del tempo, se è sotto il cursore.
    if (!selectedLayerPiece || showingSource_ || playing_)
        return nullptr;
    const int id = selectedLayerPiece();
    if (id == 0)
        return nullptr;
    const core::LayerPiece* piece = project_.textTracks().findPiece(id);
    if (piece == nullptr || position_ < piece->position - 1e-9 || position_ >= piece->endPosition() - 1e-9)
        return nullptr;
    return piece;
}

const core::TextClip* ProgramMonitor::manipText() const
{
    if (!selectedText || cropMode_ || showingSource_ || playing_)
        return nullptr;
    const int id = selectedText();
    if (id == 0)
        return nullptr;
    const core::TextClip* clip = project_.textTracks().find(id);
    if (clip == nullptr || position_ < clip->position - 1e-9 || position_ >= clip->endPosition() - 1e-9)
        return nullptr;
    return clip;
}

std::optional<ProgramMonitor::TextManip> ProgramMonitor::textManip() const
{
    const core::TextClip* clip = manipText();
    const QSize frame = frameSize();
    if (clip == nullptr || frame.isEmpty())
        return std::nullopt;
    core::TextClip form = *clip;
    form.transform = {};
    form.animation = {};
    if (boxFrame_ != frame || !(boxClip_ == form)) {
        boxRect_ = overlay_.bounds(form, frame.width(), frame.height());
        boxClip_ = form;
        boxFrame_ = frame;
    }
    if (boxRect_.isEmpty())
        return std::nullopt;
    TextManip m;
    m.clip = clip;
    m.pivot = {clip->style.centerX * frame.width(), clip->style.centerY * frame.height()};
    const double boxW = boxRect_.width() * frame.width();
    const double boxH = boxRect_.height() * frame.height();
    m.boxCenter = {boxRect_.center().x() * frame.width(), boxRect_.center().y() * frame.height()};
    m.size = {boxW, boxH, static_cast<double>(frame.width()), static_cast<double>(frame.height())};
    m.fit = std::min(m.size.projW / boxW, m.size.projH / boxH);
    return m;
}

namespace {

// Dove va a finire il vettore dal centro di stile al centro del riquadro, con scala, specchio e rotazione di `t`.
core::Point2 turnedOffset(const core::Transform& t, core::Point2 d)
{
    constexpr double kPi = 3.14159265358979323846;
    const double ex = t.scaleX * (t.flipHorizontal ? -1.0 : 1.0);
    const double ey = t.scaleY * (t.flipVertical ? -1.0 : 1.0);
    const double cs = std::cos(t.rotation * kPi / 180.0);
    const double sn = std::sin(t.rotation * kPi / 180.0);
    return {cs * ex * d.x - sn * ey * d.y, sn * ex * d.x + cs * ey * d.y};
}

}  // namespace

core::Transform ProgramMonitor::textToVirtual(const core::Transform& real, const TextManip& m) const
{
    const core::Transform t = core::normalized(real);
    const core::Point2 turned = turnedOffset(t, {m.boxCenter.x - m.pivot.x, m.boxCenter.y - m.pivot.y});
    core::Transform v = t;
    v.scaleX = t.scaleX / m.fit;
    v.scaleY = t.scaleY / m.fit;
    v.posX = m.pivot.x + t.posX + turned.x - m.size.projW / 2.0;
    v.posY = m.pivot.y + t.posY + turned.y - m.size.projH / 2.0;
    return v;
}

core::Transform ProgramMonitor::textToReal(const core::Transform& virt, const TextManip& m) const
{
    core::Transform t = virt;
    t.scaleX = virt.scaleX * m.fit;
    t.scaleY = virt.scaleY * m.fit;
    const core::Point2 turned = turnedOffset(t, {m.boxCenter.x - m.pivot.x, m.boxCenter.y - m.pivot.y});
    t.posX = m.size.projW / 2.0 + virt.posX - m.pivot.x - turned.x;
    t.posY = m.size.projH / 2.0 + virt.posY - m.pivot.y - turned.y;
    return core::normalized(t);
}

core::Transform ProgramMonitor::textShownReal() const
{
    const core::TextClip* clip = manipText();
    if (clip == nullptr)
        return {};
    return core::evaluateTransform(clip->transform, clip->animation, std::clamp(position_ - clip->position, 0.0, clip->length));
}

void ProgramMonitor::showDraggedText()
{
    // Durante il gesto sulla scritta: la scritta si disegna con la trasformazione in corso (senza animazione).
    const std::optional<TextManip> m = textManip();
    if (!m || !manipDrag_)
        return;
    for (core::TextClip& clip : shownText_) {
        if (clip.id == manipDrag_->fragmentId) {
            clip.transform = textToReal(manipDrag_->current, *m);
            clip.animation = {};
        }
    }
    player_->updateOverlay();
}

bool ProgramMonitor::manipulatorsVisible() const
{
    if (!(transformMode_ || cropMode_) || playing_ || showingSource_)
        return false;
    if (manipPiece() != nullptr || textManip())
        return true;
    return transitionId_ == 0 && gapIndex_ < 0 && index_ >= 0 && index_ < static_cast<int>(project_.sequence().fragments().size());
}

core::Transform ProgramMonitor::v1Transform() const
{
    if (manipDrag_ && !manipDrag_->layer && !manipDrag_->text)
        return manipDrag_->current;
    if (index_ < 0 || index_ >= static_cast<int>(project_.sequence().fragments().size()))
        return {};
    const core::Fragment& f = project_.sequence().fragments()[static_cast<std::size_t>(index_)];
    if (f.animation.empty())
        return f.transform;
    // Con i fotogrammi chiave la trasformazione dipende dall'istante: quello sul cursore, nei tempi del file.
    const double fileTime = f.start + std::clamp(position_ - project_.sequence().startOf(index_), 0.0, f.length());
    return core::evaluateTransform(f.transform, f.animation, fileTime);
}

core::Transform ProgramMonitor::layerPieceTransform(const core::LayerPiece& piece) const
{
    if (piece.animation.empty())
        return piece.transform;
    const double fileTime = piece.start + std::clamp(position_ - piece.position, 0.0, piece.length());
    return core::evaluateTransform(piece.transform, piece.animation, fileTime);
}

core::Transform ProgramMonitor::shownTransform() const
{
    if (manipDrag_)
        return manipDrag_->current;
    if (const core::LayerPiece* piece = manipPiece())
        return layerPieceTransform(*piece);
    if (const std::optional<TextManip> m = textManip())
        return textToVirtual(textShownReal(), *m);
    return v1Transform();
}

std::optional<ProgramMonitor::MotionPath> ProgramMonitor::motionPath() const
{
    if (!transformMode_ || cropMode_ || !manipulatorsVisible())
        return std::nullopt;
    const core::Transform* base = nullptr;
    const core::TransformAnimation* animation = nullptr;
    MotionPath path;
    double from = 0.0;
    double to = 0.0;
    const std::optional<TextManip> text = textManip();
    if (text) {
        base = &text->clip->transform;
        animation = &text->clip->animation;
        path.text = true;
        path.id = text->clip->id;
        from = 0.0;
        to = text->clip->length;
    } else if (const core::LayerPiece* piece = manipPiece()) {
        base = &piece->transform;
        animation = &piece->animation;
        path.layer = true;
        path.id = piece->id;
        from = piece->start;
        to = piece->end;
    } else {
        const std::vector<core::Fragment>& fragments = project_.sequence().fragments();
        if (index_ < 0 || index_ >= static_cast<int>(fragments.size()))
            return std::nullopt;
        const core::Fragment& f = fragments[static_cast<std::size_t>(index_)];
        base = &f.transform;
        animation = &f.animation;
        path.id = f.id;
        from = f.start;
        to = f.end;
    }
    std::vector<double> times;
    for (const core::AnimParam p : {core::AnimParam::PosX, core::AnimParam::PosY}) {
        for (const core::AnimKey& k : animation->track(p)) {
            if (k.time >= from - 1e-6 && k.time <= to + 1e-6)
                times.push_back(k.time);
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), times.end());
    if (times.size() < 2)
        return std::nullopt;
    const QSize frame = frameSize();
    const auto centerAt = [&](double t) {
        const core::Transform e = core::evaluateTransform(*base, *animation, t);
        if (text) {
            const core::Transform v = textToVirtual(e, *text);
            return core::Point2{frame.width() / 2.0 + v.posX, frame.height() / 2.0 + v.posY};
        }
        return core::Point2{frame.width() / 2.0 + e.posX, frame.height() / 2.0 + e.posY};
    };
    for (const double t : times)
        path.dots.push_back({t, centerAt(t)});
    constexpr int kSamples = 64;
    for (int i = 0; i <= kSamples; ++i)
        path.line.push_back(centerAt(times.front() + (times.back() - times.front()) * i / kSamples));
    return path;
}

int ProgramMonitor::motionDotAt(const MotionPath& path, core::Point2 p, double screenScale) const
{
    const double reach = (core::kManipHandleRadius + 2.0) / std::max(screenScale, 1e-9);
    int best = -1;
    double bestDistance = reach;
    for (std::size_t i = 0; i < path.dots.size(); ++i) {
        const double d = std::hypot(path.dots[i].pos.x - p.x, path.dots[i].pos.y - p.y);
        if (d <= bestDistance) {
            best = static_cast<int>(i);
            bestDistance = d;
        }
    }
    return best;
}

std::optional<std::array<core::Point2, 4>> ProgramMonitor::manipQuad() const
{
    const std::optional<core::ManipSize> size = manipSize();
    if (!size)
        return std::nullopt;
    return core::computeGeometry(core::normalized(shownTransform()), size->srcW, size->srcH, size->projW, size->projH).corners;
}

void ProgramMonitor::refreshManipulators()
{
    applyShownTransform();
}

std::optional<core::ManipSize> ProgramMonitor::manipSize() const
{
    if (!manipulatorsVisible())
        return std::nullopt;
    if (const std::optional<TextManip> m = textManip())
        return m->size;
    QSize source = player_->currentFrameSize();
    if (const core::LayerPiece* piece = manipPiece()) {
        // Il pezzo di un livello: la forma del suo fotogramma (conta solo quella).
        source = QSize();
        for (std::size_t i = 0; i < lastLayerIds_.size(); ++i) {
            if (lastLayerIds_[i] == piece->id && lastLayers_[i].frame != nullptr)
                source = QSize(lastLayers_[i].frame->width, lastLayers_[i].frame->height);
        }
    }
    const QSize project = frameSize();
    if (source.isEmpty() || project.isEmpty())
        return std::nullopt;
    return core::ManipSize{static_cast<double>(source.width()), static_cast<double>(source.height()),
                           static_cast<double>(project.width()), static_cast<double>(project.height())};
}

double ProgramMonitor::manipScreenScale() const
{
    const QSize project = frameSize();
    const QRectF canvas = player_->imageRect();
    return project.width() > 0 && !canvas.isEmpty() ? canvas.width() / project.width() : 1.0;
}

void ProgramMonitor::setTransformMode(bool on)
{
    if (on == transformMode_)
        return;
    transformMode_ = on;
    manipDrag_.reset();
    motionDrag_.reset();
    if (on && cropMode_) {
        setCropMode(false);
        if (onCropModeChanged)
            onCropModeChanged(false);
    }
    if (transformButton_ != nullptr && transformButton_->isChecked() != on) {
        const QSignalBlocker block(transformButton_);
        transformButton_->setChecked(on);
    }
    applyShownTransform();
}

void ProgramMonitor::setCropMode(bool on)
{
    if (on == cropMode_)
        return;
    cropMode_ = on;
    manipDrag_.reset();
    motionDrag_.reset();
    if (on && transformMode_) {
        setTransformMode(false);
        if (onTransformModeChanged)
            onTransformModeChanged(false);
    }
    if (cropButton_ != nullptr && cropButton_->isChecked() != on) {
        const QSignalBlocker block(cropButton_);
        cropButton_->setChecked(on);
    }
    applyShownTransform();
}

void ProgramMonitor::setThirdsGrid(bool on)
{
    if (on == thirdsGrid_)
        return;
    thirdsGrid_ = on;
    player_->updateOverlay();
}

void ProgramMonitor::applyShownTransform()
{
    player_->setTransformCanvas(transformMode_ || cropMode_);
    player_->setFrameTransform(v1Transform(), frameSize());
    if (manipDrag_ && manipDrag_->layer)
        showDraggedLayer();
    player_->updateOverlay();
}

void ProgramMonitor::showDraggedLayer()
{
    // Durante il gesto sul pezzo di un livello: gli stessi fotogrammi, con la trasformazione in corso.
    std::vector<PlayerWidget::LayerFrame> layers = lastLayers_;
    for (std::size_t i = 0; i < layers.size() && i < lastLayerIds_.size(); ++i) {
        if (lastLayerIds_[i] == manipDrag_->fragmentId)
            layers[i].transform = manipDrag_->current;
    }
    player_->setLayers(std::move(layers), frameSize());
}

void ProgramMonitor::paintManipulators(QPainter& painter, const QRectF& target)
{
    const std::optional<core::ManipSize> size = manipSize();
    if (!size || target.isEmpty())
        return;
    const core::Transform t = shownTransform();
    const core::TransformGeometry g = core::computeGeometry(core::normalized(t), size->srcW, size->srcH, size->projW, size->projH);
    const double scale = target.width() / size->projW;
    const auto toWidget = [&](core::Point2 p) { return QPointF(target.left() + p.x * scale, target.top() + p.y * scale); };
    QPolygonF quad;
    for (const core::Point2& corner : g.corners)
        quad << toWidget(corner);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Il riquadro: una linea chiara con un'ombra scura sotto, per vedersi su qualsiasi immagine.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
    painter.drawPolygon(quad);
    painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
    painter.drawPolygon(quad);
    const auto handle = [&](const QPointF& at, bool round) {
        const double r = core::kManipHandleRadius * 0.7;
        painter.setPen(QPen(QColor(0, 0, 0, 200), 1.5));
        painter.setBrush(QColor(255, 255, 255));
        if (round)
            painter.drawEllipse(at, r, r);
        else
            painter.drawRect(QRectF(at.x() - r, at.y() - r, 2 * r, 2 * r));
    };
    if (const std::optional<MotionPath> path = motionDrag_ ? std::optional<MotionPath>(motionDrag_->path) : motionPath()) {
        // Il percorso del movimento: la curva e un punto per chiave (quello preso segue il mouse).
        QPolygonF line;
        std::vector<core::Point2> points = path->line;
        if (motionDrag_) {  // durante il trascinamento: segmenti fra i punti, con quello preso dov'è il mouse
            points.clear();
            for (std::size_t i = 0; i < path->dots.size(); ++i)
                points.push_back(i == motionDrag_->dot ? motionDrag_->current : path->dots[i].pos);
        }
        for (const core::Point2& p : points)
            line << toWidget(p);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3, Qt::DashLine));
        painter.drawPolyline(line);
        painter.setPen(QPen(QColor(255, 170, 0, 240), 1.5, Qt::DashLine));
        painter.drawPolyline(line);
        for (std::size_t i = 0; i < path->dots.size(); ++i) {
            const QPointF at = toWidget(motionDrag_ && i == motionDrag_->dot ? motionDrag_->current : path->dots[i].pos);
            const double r = core::kManipHandleRadius * 0.85;
            painter.setPen(QPen(QColor(0, 0, 0, 200), 1.5));
            painter.setBrush(QColor(255, 170, 0));
            QPolygonF diamond;
            diamond << QPointF(at.x(), at.y() - r) << QPointF(at.x() + r, at.y()) << QPointF(at.x(), at.y() + r) << QPointF(at.x() - r, at.y());
            painter.drawPolygon(diamond);
        }
    }
    if (cropMode_) {
        // Il file intero, tratteggiato, per vedere cosa si è tolto; le maniglie sugli angoli e a metà dei lati del riquadro.
        QPolygonF full;
        for (const core::Point2& corner : g.fullCorners)
            full << toWidget(corner);
        painter.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DashLine));
        painter.drawPolygon(full);
        for (const int edges : {core::kCropLeft | core::kCropTop, core::kCropRight | core::kCropTop,
                                core::kCropRight | core::kCropBottom, core::kCropLeft | core::kCropBottom})
            handle(toWidget(core::cropHandlePoint(t, *size, edges)), false);
        for (const int edge : {core::kCropLeft, core::kCropRight, core::kCropTop, core::kCropBottom})
            handle(toWidget(core::cropHandlePoint(t, *size, edge)), true);
    } else {
        // La maniglia di rotazione, collegata al lato alto.
        const QPointF top = (quad[0] + quad[1]) / 2.0;
        const QPointF rotate = toWidget(core::manipRotationHandle(t, *size, scale));
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
        painter.drawLine(top, rotate);
        painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
        painter.drawLine(top, rotate);
        for (const QPointF& corner : quad)
            handle(corner, false);
        handle(rotate, true);
    }
    // Le guide su cui sta scattando il gesto.
    painter.setPen(QPen(QColor(255, 80, 200, 230), 1));
    for (const double x : snapGuides_.vertical)
        painter.drawLine(QPointF(target.left() + x * scale, target.top()), QPointF(target.left() + x * scale, target.bottom()));
    for (const double y : snapGuides_.horizontal)
        painter.drawLine(QPointF(target.left(), target.top() + y * scale), QPointF(target.right(), target.top() + y * scale));
    painter.restore();
}

bool ProgramMonitor::grabImage(QPointF at)
{
    if (onActivated)
        onActivated();
    const std::optional<core::ManipSize> size = manipSize();
    if (!size)
        return grabText(at);
    const core::Point2 p{at.x() * size->projW, at.y() * size->projH};
    const core::Transform start = shownTransform();
    snapGuides_ = {};
    rotationSnapped_ = false;
    const core::LayerPiece* layerPiece = manipPiece();
    const core::TextClip* textClip = layerPiece == nullptr ? manipText() : nullptr;
    const int targetId = layerPiece != nullptr ? layerPiece->id
                         : textClip != nullptr ? textClip->id
                                               : project_.sequence().fragments()[static_cast<std::size_t>(index_)].id;
    if (cropMode_) {
        const int edges = core::cropHitTest(start, *size, p, manipScreenScale());
        if (edges == 0)
            return grabText(at);
        manipDrag_ = ManipDrag{targetId, {}, start, start, p, edges, layerPiece != nullptr, false};
        return true;
    }
    // Prima i punti del percorso del movimento.
    if (const std::optional<MotionPath> path = motionPath()) {
        if (const int dot = motionDotAt(*path, p, manipScreenScale()); dot >= 0) {
            motionDrag_ = MotionDrag{*path, static_cast<std::size_t>(dot), path->dots[static_cast<std::size_t>(dot)].pos};
            return true;
        }
    }
    const core::ManipHit hit = core::manipHitTest(start, *size, p, manipScreenScale());
    const auto startDrag = [&] {
        manipDrag_ = ManipDrag{targetId, hit, start, start, p, 0, layerPiece != nullptr, textClip != nullptr};
        return true;
    };
    if (textClip != nullptr) {
        // La scritta scelta: le sue maniglie e il suo riquadro (si sposta da lì); fuori, un'altra scritta si può afferrare.
        if (hit.part != core::ManipPart::None)
            return startDrag();
        return grabText(at);
    }
    // Prima le maniglie, poi le scritte (stanno sopra l'immagine), poi il resto del pezzo.
    if (hit.part == core::ManipPart::Corner || hit.part == core::ManipPart::Rotate)
        return startDrag();
    if (grabText(at))
        return true;
    if (hit.part == core::ManipPart::Move)
        return startDrag();
    return false;
}

void ProgramMonitor::dragImage(QPointF at)
{
    if (motionDrag_) {
        if (const std::optional<core::ManipSize> size = manipSize()) {
            motionDrag_->current = {at.x() * size->projW, at.y() * size->projH};
            player_->update();
        }
        return;
    }
    if (!manipDrag_) {
        dragText(at);
        return;
    }
    const std::optional<core::ManipSize> size = manipSize();
    if (!size)
        return;
    const core::Point2 p{at.x() * size->projW, at.y() * size->projH};
    const Qt::KeyboardModifiers keys = QGuiApplication::keyboardModifiers();
    const bool shift = keys.testFlag(Qt::ShiftModifier);
    const bool alt = keys.testFlag(Qt::AltModifier);
    // Ctrl: niente scatti magnetici.
    const bool snap = !keys.testFlag(Qt::ControlModifier);
    snapGuides_ = {};
    if (cropMode_) {
        manipDrag_->current = core::manipCrop(manipDrag_->start, *size, manipDrag_->cropEdges, p);
    } else {
        switch (manipDrag_->hit.part) {
        case core::ManipPart::Move:
            manipDrag_->current = core::manipMove(manipDrag_->start, manipDrag_->from, p);
            if (snap)
                manipDrag_->current = core::snapMove(manipDrag_->current, *size, manipScreenScale(), &snapGuides_);
            break;
        case core::ManipPart::Corner:
            manipDrag_->current = core::manipScale(manipDrag_->start, *size, manipDrag_->hit.corner, p, shift, alt);
            if (snap && !manipDrag_->text)  // (per una scritta il 100% non è l'adattamento al fotogramma)
                manipDrag_->current = core::snapScale(manipDrag_->current);
            break;
        case core::ManipPart::Rotate:
            manipDrag_->current = core::manipRotate(manipDrag_->start, *size, manipDrag_->from, p, shift);
            if (snap && !shift)
                manipDrag_->current = core::snapRotation(manipDrag_->current, &rotationSnapped_);
            break;
        case core::ManipPart::None:
            break;
        }
    }
    if (manipDrag_->layer)
        showDraggedLayer();
    else if (manipDrag_->text)
        showDraggedText();
    else
        player_->setFrameTransform(manipDrag_->current, frameSize());
    player_->updateOverlay();
}

void ProgramMonitor::dropImage(QPointF at)
{
    if (motionDrag_) {
        dragImage(at);
        const MotionDrag done = *motionDrag_;
        motionDrag_.reset();
        const QSize frame = frameSize();
        const MotionDot& dot = done.path.dots[done.dot];
        if (std::abs(done.current.x - dot.pos.x) > 0.5 || std::abs(done.current.y - dot.pos.y) > 0.5) {
            if (done.path.text) {
                // Il punto è il centro del riquadro: lo spostamento vero è quello che ve lo porta, con scala e rotazione di quell'istante.
                if (const std::optional<TextManip> m = textManip(); m && onTextMotionKeyMoved) {
                    const core::Transform e = core::evaluateTransform(m->clip->transform, m->clip->animation, dot.fileTime);
                    core::Transform v = textToVirtual(e, *m);
                    v.posX = done.current.x - frame.width() / 2.0;
                    v.posY = done.current.y - frame.height() / 2.0;
                    const core::Transform real = textToReal(v, *m);
                    onTextMotionKeyMoved(done.path.id, dot.fileTime, real.posX, real.posY);
                }
            } else if (onMotionKeyMoved) {
                onMotionKeyMoved(done.path.layer, done.path.id, dot.fileTime, done.current.x - frame.width() / 2.0,
                                 done.current.y - frame.height() / 2.0);
            }
        }
        applyShownTransform();
        player_->update();
        return;
    }
    if (!manipDrag_) {
        dropText(at);
        return;
    }
    dragImage(at);
    const ManipDrag done = *manipDrag_;
    manipDrag_.reset();
    motionDrag_.reset();
    snapGuides_ = {};
    rotationSnapped_ = false;
    if (!(done.current == done.start)) {
        if (done.text) {
            if (const std::optional<TextManip> m = textManip(); m && onTextTransformEdited)
                onTextTransformEdited(done.fragmentId, textToReal(done.current, *m));
        } else if (done.layer && onLayerTransformEdited)
            onLayerTransformEdited(done.fragmentId, done.current);
        else if (!done.layer && onTransformEdited)
            onTransformEdited(done.fragmentId, done.current);
    }
    // Se il progetto non ha cambiato il pezzo, il monitor torna com'era.
    if (done.layer)
        player_->setLayers(lastLayers_, frameSize());
    if (done.text)
        updateTextOverlay();
    applyShownTransform();
}

bool ProgramMonitor::grabText(QPointF at)
{
    if (playing_ || showingSource_ || shownText_.empty())
        return false;
    // La più alta (l'ultima disegnata) che contiene il punto.
    const QSize frame = frameSize();
    for (int i = static_cast<int>(shownText_.size()) - 1; i >= 0; --i) {
        const core::TextClip& clip = shownText_[static_cast<std::size_t>(i)];
        if (overlay_.bounds(clip, frame.width(), frame.height()).adjusted(-0.01, -0.01, 0.01, 0.01).contains(at)) {
            textDrag_ = TextDrag{i, QPointF(clip.style.centerX, clip.style.centerY) - at};
            if (onTextGrabbed)
                onTextGrabbed(clip.id);
            return true;
        }
    }
    return false;
}

void ProgramMonitor::dragText(QPointF at)
{
    if (!textDrag_)
        return;
    core::TextClip& clip = shownText_[static_cast<std::size_t>(textDrag_->index)];
    clip.style.centerX = std::clamp(at.x() + textDrag_->grabOffset.x(), 0.0, 1.0);
    clip.style.centerY = std::clamp(at.y() + textDrag_->grabOffset.y(), 0.0, 1.0);
    player_->updateOverlay();
}

void ProgramMonitor::dropText(QPointF at)
{
    if (!textDrag_)
        return;
    dragText(at);
    const core::TextClip clip = shownText_[static_cast<std::size_t>(textDrag_->index)];
    textDrag_.reset();
    if (onTextDragged)
        onTextDragged(clip.id, clip.style.centerX, clip.style.centerY);
    updateTextOverlay();  // se la modifica non è stata fatta, le scritte tornano come nel progetto
}

void ProgramMonitor::updateTextOverlay()
{
    if (textDrag_)
        return;
    std::vector<core::TextClip> now = visibleText();
    // Una scritta in dissolvenza cambia a ogni movimento del cursore, le altre solo quando cambiano (o compaiono).
    const bool fading = std::any_of(now.begin(), now.end(), [&](const core::TextClip& c) { return c.fadeFactor(position_) < 1.0 || !c.animation.empty(); });
    std::vector<int> levels;
    for (const core::TextClip& c : now)
        levels.push_back(project_.textTracks().levelOf(c.id));
    if (now == shownText_ && levels == shownLevels_ && !fading && !shownFading_)
        return;
    shownLevels_ = std::move(levels);
    shownText_ = std::move(now);
    shownTime_ = position_;
    shownFading_ = fading;
    player_->updateOverlay();
}

void ProgramMonitor::updateLayers()
{
    constexpr double kPrime = 1.0;  // quanto prima si apre il file di un pezzo che sta per iniziare, in riproduzione
    std::vector<PlayerWidget::LayerFrame> shown;
    std::vector<int> wanted;
    std::vector<int> shownIds;
    if (!showingSource_) {
        const QSize project = frameSize();
        const QSizeF fitted = QSizeF(project).scaled(QSizeF(1280, 720), Qt::KeepAspectRatio);
        const int boxWidth = std::min(project.width(), std::max(2, static_cast<int>(fitted.width())));
        const int boxHeight = std::min(project.height(), std::max(2, static_cast<int>(fitted.height())));
        for (const core::TextLayer& layer : project_.textTracks().layers()) {
            for (const core::LayerPiece& piece : layer.pieces) {
                const bool active = position_ >= piece.position - 1e-9 && position_ < piece.endPosition() - 1e-9;
                const bool coming = playing_ && speed_ > 0 && !active && piece.position > position_ && piece.position - position_ < kPrime;
                if (!active && !coming)
                    continue;
                const core::Clip* clip = project_.find(piece.clipId);
                if (clip == nullptr)
                    continue;
                std::string path = clip->path;
                if (player_->videoSourceFor)
                    path = player_->videoSourceFor(QString::fromStdString(clip->path)).toStdString();
                std::unique_ptr<core::LayerPlayback>& slot = layerPlaybacks_[piece.id];
                if (!slot || slot->path() != path)
                    slot = std::make_unique<core::LayerPlayback>(path, boxWidth, boxHeight);
                wanted.push_back(piece.id);
                if (coming) {
                    slot->request(piece.start, false);  // il primo fotogramma, pronto prima che serva
                    continue;
                }
                const double fileTime = piece.start + (position_ - piece.position);
                std::shared_ptr<core::VideoFrame> frame;
                if (playing_) {
                    slot->request(fileTime, true);
                    frame = slot->frame(fileTime);
                } else {
                    frame = slot->waitFrame(fileTime, 800);
                }
                shownIds.push_back(piece.id);
                shown.push_back({std::move(frame), layerPieceTransform(piece), project_.textTracks().levelOf(piece.id)});
            }
        }
    }
    for (auto it = layerPlaybacks_.begin(); it != layerPlaybacks_.end();) {
        it = std::find(wanted.begin(), wanted.end(), it->first) == wanted.end() ? layerPlaybacks_.erase(it) : std::next(it);
    }
    const bool any = !shown.empty();
    layerFramesShown_ = static_cast<int>(std::count_if(shown.begin(), shown.end(), [](const PlayerWidget::LayerFrame& l) { return l.frame != nullptr; }));
    lastLayers_ = shown;
    lastLayerIds_ = std::move(shownIds);
    player_->setLayers(std::move(shown), frameSize());
    if (any != layersShown_) {
        layersShown_ = any;
        player_->setTransformCanvas(transformMode_ || cropMode_ || layersShown_);
        // (cambiare il modo di disegno fa ricalcolare la trasformazione: V1 si rimette com'era, se c'è un suo pezzo)
        if (!showingSource_ && transitionId_ == 0 && gapIndex_ < 0 && index_ >= 0
            && index_ < static_cast<int>(project_.sequence().fragments().size()))
            applyShownTransform();
    }
}

QSize ProgramMonitor::frameSize() const
{
    // Le misure del fotogramma del progetto; senza formato, 16:9.
    const core::StreamSignature* video = project_.format().video();
    return video != nullptr && video->width > 0 && video->height > 0 ? QSize(video->width, video->height) : QSize(1920, 1080);
}

std::optional<core::TransitionPlacement> ProgramMonitor::transitionAt(double seconds) const
{
    if (project_.transitions().empty())
        return std::nullopt;
    for (const core::TransitionPlacement& placement : project_.activeTransitions()) {
        if (seconds >= placement.start() - 1e-6 && seconds < placement.end() - 1e-6)
            return placement;
    }
    return std::nullopt;
}

void ProgramMonitor::enterTransition(const core::TransitionPlacement& placement, double seconds, bool wait)
{
    if (player_->isPlaying())
        player_->pause();
    player_->setBlank(false);
    player_->setFrameTransform({}, {});  // i fotogrammi della transizione sono già fatti (con le trasformazioni dei due pezzi)
    index_ = -1;
    gapIndex_ = -1;
    transitionId_ = placement.id;
    position_ = seconds;
    gapBase_ = seconds;
    gapClock_.restart();
    transitionFrom_ = placement.start();
    transitionTo_ = placement.end();
    showTransitionFrame(placement, seconds, wait);
}

void ProgramMonitor::setTransitionBox()
{
    // Le anteprime stanno in 1280x720 al massimo: due decodifiche e la fusione devono stare nel tempo di un fotogramma.
    QSize box = frameSize();
    if (box.width() > 1280 || box.height() > 720)
        box = box.scaled(1280, 720, Qt::KeepAspectRatio);
    transitionPlayback_.setBox(box.width(), box.height());
}

void ProgramMonitor::showTransitionFrame(const core::TransitionPlacement& placement, double seconds, bool wait)
{
    const std::optional<core::TransitionJob> job = core::makeTransitionJob(project_, placement);
    if (!job)
        return;
    setTransitionBox();
    if (wait) {
        transitionFrame_ = transitionPlayback_.render(*job, seconds);
        if (transitionFrame_)
            player_->showExternalFrame(transitionFrame_);
        return;
    }
    // In riproduzione: il thread prepara i fotogrammi in anticipo, al ritmo del video, e il timer veloce mostra quello
    // del momento (l'orologio è quello dell'audio, come per il lettore).
    const double step = std::clamp(player_->frameDuration(), 1.0 / 60, 1.0 / 12) * std::max(1.0, std::abs(speed_));
    transitionPlayback_.startStream(*job, seconds, step, speed_ < 0 ? -1 : 1);
    transitionTimer_->start();
}

double ProgramMonitor::transitionClock() const
{
    if (playing_ && sequenceAudio_.running())
        return sequenceAudio_.position();
    return gapBase_ + speed_ * static_cast<double>(gapClock_.nsecsElapsed()) / 1e9;
}

void ProgramMonitor::transitionTick()
{
    if (transitionId_ == 0 || !playing_) {
        transitionTimer_->stop();
        return;
    }
    const double time = std::clamp(transitionClock(), transitionFrom_, transitionTo_);
    const std::shared_ptr<core::VideoFrame> frame = transitionPlayback_.takeFrame(time);
    if (frame && frame != transitionFrame_) {
        transitionFrame_ = frame;
        player_->showExternalFrame(frame);
    }
}

void ProgramMonitor::primeTransition()
{
    // Aprire due file e posizionarli richiede un po': lo si fa un secondo prima, mentre il pezzo suona ancora.
    if (project_.transitions().empty() || speed_ == 0.0)
        return;
    const double window = std::max(1.0, std::abs(speed_));
    for (const core::TransitionPlacement& placement : project_.activeTransitions()) {
        const bool ahead = speed_ > 0 ? placement.start() > position_ && placement.start() - position_ <= window
                                      : placement.end() < position_ && position_ - placement.end() <= window;
        if (!ahead || placement.id == primedTransitionId_)
            continue;
        if (const std::optional<core::TransitionJob> job = core::makeTransitionJob(project_, placement)) {
            primedTransitionId_ = placement.id;
            setTransitionBox();
            transitionPlayback_.request(*job, speed_ > 0 ? placement.start() : placement.end() - 1e-3);
        }
        return;
    }
}

void ProgramMonitor::updateOverlayFrame()
{
    const QSize frame = frameSize();
    player_->setTextPainter([this, frame](QPainter& painter, const QRectF& target, int from, int to) {
        std::vector<core::TextClip> clips;
        for (std::size_t i = 0; i < shownText_.size(); ++i) {
            const int level = i < shownLevels_.size() ? shownLevels_[i] : 0;
            if (level >= from && level < to)
                clips.push_back(shownText_[i]);
        }
        if (!clips.empty())
            overlay_.paint(painter, target, painter.device()->devicePixelRatioF(), clips, shownTime_, frame);
    });
    player_->setOverlay(
        [this](QPainter& painter, const QRectF& target) {
            if (thirdsGrid_ && !target.isEmpty()) {
                painter.save();
                painter.setPen(QPen(QColor(255, 255, 255, 110), 1));
                for (int i = 1; i <= 2; ++i) {
                    const double x = target.left() + target.width() * i / 3.0;
                    const double y = target.top() + target.height() * i / 3.0;
                    painter.drawLine(QPointF(x, target.top()), QPointF(x, target.bottom()));
                    painter.drawLine(QPointF(target.left(), y), QPointF(target.right(), y));
                }
                painter.restore();
            }
            paintManipulators(painter, target);
        },
        frame);
}

int ProgramMonitor::currentClipId() const
{
    const auto& fragments = project_.sequence().fragments();
    if (transitionId_ != 0) {
        // Nel tratto di una transizione: il file del pezzo che finisce.
        if (const core::TransitionClip* transition = project_.transitions().find(transitionId_))
            if (const core::Fragment* before = project_.sequence().find(transition->beforeFragment))
                return before->clipId;
    }
    return index_ >= 0 && index_ < static_cast<int>(fragments.size()) ? fragments[static_cast<std::size_t>(index_)].clipId : 0;
}

void ProgramMonitor::showSourceFrame(int clipId, double sourceTime, const QString& note)
{
    const core::Clip* clip = project_.find(clipId);
    if (clip == nullptr)
        return;
    if (playing_) {
        player_->pause();
        stopAudio();
        playing_ = false;
        poll_->stop();
        updateLabels();
    }
    gapIndex_ = -1;
    transitionId_ = 0;
    transitionFrame_.reset();
    index_ = -1;  // nessun pezzo: il prossimo seekTo riapre quello giusto
    showingSource_ = true;
    updateLayers();
    updateTextOverlay();
    player_->setBlank(false);
    player_->setFrameTransform({}, {});  // un fotogramma del file, com'è
    if (clipId != loadedClipId_) {
        player_->openFile(QString::fromStdString(clip->path));
        loadedClipId_ = clipId;
    }
    player_->seekTo(sourceTime);
    fragmentLabel_->setText(note);
}

bool ProgramMonitor::hasProgram() const
{
    return !project_.sequence().empty() || project_.textTracks().endTime() > 0.0 || project_.audioTracks().endTime() > 0.0;
}

double ProgramMonitor::programEnd() const
{
    // Il programma dura fino alla fine della traccia più lunga: V1, scritte e livelli, clip audio.
    return std::max({project_.sequence().totalLength(), project_.textTracks().endTime(), project_.audioTracks().endTime()});
}

void ProgramMonitor::seekTo(double seconds)
{
    showingSource_ = false;
    const core::Sequence& sequence = project_.sequence();
    if (!hasProgram())
        return;
    const double t = std::clamp(seconds, 0.0, programEnd());
    if (const auto placement = transitionAt(t)) {
        // Nel tratto di una transizione si vedono i due pezzi fusi: li fa il monitor, non il lettore.
        const bool wasPlaying = playing_;
        if (wasPlaying) {
            player_->pause();
            stopAudio();
        }
        enterTransition(*placement, t, !wasPlaying);
        if (wasPlaying)
            startAudio(t);
        updatePosition();
        return;
    }
    int index = sequence.indexAt(t);
    if (index < 0) {
        if (const int gap = sequence.gapIndexAt(t); gap >= 0) {
            if (playing_) {
                player_->pause();
                startAudio(t);  // l'audio continua anche nello spazio vuoto
            }
            enterGap(gap, t);
            return;
        }
        if (sequence.fragments().empty() || t > sequence.totalLength() + 1e-6) {
            // Oltre l'ultimo pezzo ma dentro scritte o livelli: coda nera.
            if (playing_) {
                player_->pause();
                startAudio(t);
            }
            enterGap(static_cast<int>(sequence.fragments().size()), t);
            return;
        }
        index = static_cast<int>(sequence.fragments().size()) - 1;  // alla fine: ultimo pezzo
    }
    const bool wasPlaying = playing_;
    if (wasPlaying) {
        player_->pause();
        stopAudio();
    }
    load(index, t - sequence.startOf(index));
    if (wasPlaying) {
        startAudio(t);
        player_->play();
    }
    updatePosition();
}

void ProgramMonitor::enterGap(int index, double seconds)
{
    if (player_->isPlaying())
        player_->pause();
    player_->setBlank(true);
    index_ = -1;
    transitionId_ = 0;
    transitionFrame_.reset();
    gapIndex_ = index;
    position_ = seconds;
    gapBase_ = seconds;
    gapClock_.restart();
    updatePosition();
}

void ProgramMonitor::seekRelative(double deltaSeconds)
{
    seekTo(position_ + deltaSeconds);
}

void ProgramMonitor::togglePlayPause()
{
    const core::Sequence& sequence = project_.sequence();
    if (!hasProgram())
        return;
    if (playing_) {
        updatePosition();  // nello spazio vuoto: il tempo arrivato fin qui
        player_->pause();
        stopAudio();
        playing_ = false;
        poll_->stop();
        setCurrentSpeed(baseSpeed_);  // finisce la spola
        updatePosition();
        return;
    }
    playStart_ = position_;
    hasPlayStart_ = true;
    start();
}

void ProgramMonitor::start()
{
    const core::Sequence& sequence = project_.sequence();
    if (!hasProgram() || playing_)
        return;
    if (speed_ < 0) {
        if (position_ <= 0.0005) {
            setCurrentSpeed(baseSpeed_);  // all'indietro dall'inizio: niente da riprodurre
            return;
        }
        if (index_ < 0 && gapIndex_ < 0)
            seekTo(position_);
    } else if (position_ >= programEnd() - 0.05) {
        seekTo(0.0);  // alla fine: si ricomincia
    } else if (index_ < 0) {
        seekTo(position_);
    }
    playing_ = true;
    if (onPlaybackStarted)
        onPlaybackStarted();
    startAudio(position_);
    primedTransitionId_ = 0;
    if (gapIndex_ >= 0 || transitionId_ != 0) {
        gapBase_ = position_;  // nello spazio vuoto e nella transizione il tempo lo dà l'audio o l'orologio
        gapClock_.restart();
    } else {
        player_->play();
    }
    poll_->start();
    updateLabels();
}

void ProgramMonitor::setPlaying(bool playing)
{
    if (playing != playing_)
        togglePlayPause();
}

void ProgramMonitor::stop()
{
    if (!hasProgram())
        return;
    if (playing_)
        togglePlayPause();
    setCurrentSpeed(baseSpeed_);
    if (hasPlayStart_)
        seekTo(playStart_);
}

void ProgramMonitor::setBaseSpeed(double speed)
{
    baseSpeed_ = speed > 0 ? speed : 1.0;
    setCurrentSpeed(baseSpeed_);
}

void ProgramMonitor::setCurrentSpeed(double speed)
{
    if (speed == 0.0)
        return;
    if ((gapIndex_ >= 0 || transitionId_ != 0) && playing_) {
        updatePosition();  // nello spazio vuoto (o nella transizione) il tempo riparte da qui con la nuova velocità
        gapBase_ = position_;
        gapClock_.restart();
    }
    // Il mix riparte da qui alla nuova velocità (a quelle in cui non si sente, e all'indietro, si va a tempo reale).
    const bool restartAudio = playing_ && speed != speed_;
    const bool playerRunning = player_->isPlaying();
    if (restartAudio) {
        updatePosition();
        if (playerRunning)
            player_->pause();
        stopAudio();
    }
    speed_ = speed;
    player_->setSpeed(speed);
    if (restartAudio) {
        startAudio(position_);
        if (playerRunning)
            player_->play();
    }
    updateLabels();
}

void ProgramMonitor::shuttle(int direction)
{
    if (!hasProgram())
        return;
    setCurrentSpeed(core::shuttleSpeed(speed_, playing_, direction));
    if (!playing_) {
        playStart_ = position_;
        hasPlayStart_ = true;
        start();
    }
}

void ProgramMonitor::setLoop(bool loop)
{
    loop_ = loop;
    updateLabels();
}

void ProgramMonitor::setVolume(int percent)
{
    volume_ = std::clamp(percent, 0, 100);
    if (volume_ > 0)
        muted_ = false;
    applyVolume();
    updateLabels();
}

void ProgramMonitor::setMuted(bool muted)
{
    muted_ = muted;
    applyVolume();
    updateLabels();
}

void ProgramMonitor::stepFrame(int direction)
{
    if (!hasProgram())
        return;
    setPlaying(false);
    // Un fotogramma del file mostrato; a cavallo di una giunzione seekTo() passa al pezzo giusto.
    seekTo(position_ + direction * player_->frameDuration() + 0.0005);
}

void ProgramMonitor::stepToCut(int direction)
{
    const core::Sequence& sequence = project_.sequence();
    if (!hasProgram())
        return;
    setPlaying(false);
    const std::vector<double> starts = sequence.starts();
    const int count = static_cast<int>(starts.size());
    double target = position_;
    if (direction > 0) {
        target = programEnd();
        for (int i = 0; i < count; ++i) {
            if (starts[static_cast<std::size_t>(i)] > position_ + 0.001) {
                target = starts[static_cast<std::size_t>(i)];
                break;
            }
        }
        if (target >= programEnd())
            return seekToEnd();
    } else {
        target = 0.0;
        for (int i = count - 1; i >= 0; --i) {
            if (starts[static_cast<std::size_t>(i)] < position_ - 0.001) {
                target = starts[static_cast<std::size_t>(i)];
                break;
            }
        }
    }
    seekTo(target);
}

void ProgramMonitor::seekToEnd()
{
    const core::Sequence& sequence = project_.sequence();
    if (!hasProgram())
        return;
    setPlaying(false);
    seekTo(std::max(0.0, programEnd() - player_->frameDuration()));
}

void ProgramMonitor::fragmentEnded()
{
    if (!playing_)
        return;
    const core::Sequence& sequence = project_.sequence();
    const auto& fragments = sequence.fragments();
    if (speed_ < 0) {
        // All'indietro: lo spazio vuoto prima di questo pezzo, oppure la fine del pezzo prima.
        if (index_ < 0 || index_ >= static_cast<int>(fragments.size()))
            return reachedEdge();
        const double gap = fragments[static_cast<std::size_t>(index_)].gap;
        if (gap > 0.0) {
            enterGap(index_, sequence.startOf(index_) - 0.0005);
            return;
        }
        if (index_ == 0)
            return reachedEdge();
        const int previous = index_ - 1;
        load(previous, fragments[static_cast<std::size_t>(previous)].length() - 0.001);
        player_->play();
        updatePosition();
        return;
    }
    const int next = index_ + 1;
    if (next < static_cast<int>(fragments.size())) {
        const double gap = fragments[static_cast<std::size_t>(next)].gap;
        if (gap > 0.0) {
            enterGap(next, sequence.startOf(next) - gap);  // prima del pezzo dopo c'è uno spazio vuoto
            return;
        }
        load(next, 0.0);
        player_->play();
    } else if (programEnd() > sequence.totalLength() + 1e-6) {
        enterGap(static_cast<int>(fragments.size()), sequence.totalLength());  // coda con scritte o livelli
        return;
    } else {
        return reachedEdge();
    }
    updatePosition();
}

void ProgramMonitor::reachedEdge()
{
    const core::Sequence& sequence = project_.sequence();
    if (loop_ && hasProgram()) {
        // Ripetizione: si riparte dall'altro capo del montaggio, senza fermarsi.
        seekTo(speed_ < 0 ? std::max(0.0, programEnd() - player_->frameDuration()) : 0.0);
        if (gapIndex_ >= 0) {
            gapBase_ = position_;
            gapClock_.restart();
        } else if (!player_->isPlaying()) {
            player_->play();
        }
        updatePosition();
        return;
    }
    playing_ = false;  // fine (o inizio) del montaggio
    stopAudio();
    poll_->stop();
    if (speed_ < 0)
        seekTo(0.0);
    setCurrentSpeed(baseSpeed_);
    updatePosition();
}

void ProgramMonitor::sequenceChanged()
{
    showingSource_ = false;
    updateOverlayFrame();
    const core::Sequence& sequence = project_.sequence();
    if (playing_) {
        player_->pause();
        stopAudio();
        playing_ = false;
        poll_->stop();
    }
    transitionId_ = 0;  // seekTo() rientra nella transizione se il punto ci cade ancora
    primedTransitionId_ = 0;
    transitionFrame_.reset();
    transitionPlayback_.reset();  // i file o le durate possono essere cambiati
    setCurrentSpeed(baseSpeed_);
    if (loadedClipId_ != 0 && project_.find(loadedClipId_) == nullptr)
        loadedClipId_ = 0;
    if (!hasProgram()) {
        index_ = -1;
        gapIndex_ = -1;
        player_->setBlank(false);
        position_ = 0.0;
        loadedClipId_ = 0;
        player_->closeFile();
        updatePosition();
        return;
    }
    if (sequence.empty()) {
        // Niente su V1, ma ci sono scritte o livelli: si parte da una coda nera.
        index_ = -1;
        loadedClipId_ = 0;
        player_->closeFile();
    }
    seekTo(std::min(position_, programEnd()));
}

void ProgramMonitor::updatePosition()
{
    const core::Sequence& sequence = project_.sequence();
    if (transitionId_ != 0) {
        const auto placement = project_.transitionPlacement(transitionId_);
        if (!placement) {
            transitionId_ = 0;  // la transizione non c'è più (la sequenza è cambiata): sequenceChanged() riposiziona
            transitionFrame_.reset();
        } else {
            const double from = placement->start();
            const double to = placement->end();
            if (playing_ && sequenceAudio_.running())
                position_ = std::clamp(sequenceAudio_.position(), from, to);  // lo scandisce l'audio
            else if (playing_)
                position_ = std::clamp(gapBase_ + speed_ * static_cast<double>(gapClock_.nsecsElapsed()) / 1e9, from, to);
            if (playing_ && speed_ > 0 && position_ >= to - 1e-6) {
                // Finita la transizione: il pezzo dopo, da dove siamo arrivati.
                const int next = sequence.indexOf(placement->afterFragment);
                transitionId_ = 0;
                transitionFrame_.reset();
                if (next >= 0) {
                    load(next, std::max(0.0, to - sequence.startOf(next)));
                    player_->play();
                }
            } else if (playing_ && speed_ < 0 && position_ <= from + 1e-6) {
                // All'indietro: la fine del pezzo prima, da dove siamo arrivati.
                const int previous = sequence.indexOf(placement->beforeFragment);
                transitionId_ = 0;
                transitionFrame_.reset();
                if (previous >= 0) {
                    load(previous, std::max(0.0, from - sequence.startOf(previous) - 0.001));
                    player_->play();
                }
            }
        }
    } else if (gapIndex_ >= 0 && gapIndex_ == static_cast<int>(sequence.fragments().size())) {
        // Coda dopo l'ultimo pezzo: nera, con le scritte e i livelli che arrivano più in là.
        const double gapStart = sequence.totalLength();
        const double gapEnd = programEnd();
        if (playing_ && sequenceAudio_.running())
            position_ = std::clamp(sequenceAudio_.position(), gapStart, gapEnd);
        else if (playing_)
            position_ = std::clamp(gapBase_ + speed_ * static_cast<double>(gapClock_.nsecsElapsed()) / 1e9, gapStart, gapEnd);
        if (playing_ && speed_ > 0 && position_ >= gapEnd - 1e-6) {
            position_ = gapEnd;
            reachedEdge();
            return;
        } else if (playing_ && speed_ < 0 && position_ <= gapStart + 1e-6 && gapIndex_ > 0) {
            const int previous = gapIndex_ - 1;
            load(previous, sequence.fragments()[static_cast<std::size_t>(previous)].length() - 0.001);
            player_->play();
        }
    } else if (gapIndex_ >= 0 && gapIndex_ < static_cast<int>(sequence.fragments().size())) {
        const double gapEnd = sequence.startOf(gapIndex_);
        const double gapStart = gapEnd - sequence.fragments()[static_cast<std::size_t>(gapIndex_)].gap;
        if (playing_ && sequenceAudio_.running())
            position_ = std::clamp(sequenceAudio_.position(), gapStart, gapEnd);  // lo scandisce l'audio
        else if (playing_)
            position_ = std::clamp(gapBase_ + speed_ * static_cast<double>(gapClock_.nsecsElapsed()) / 1e9, gapStart, gapEnd);
        if (playing_ && speed_ > 0 && position_ >= gapEnd - 1e-6) {
            load(gapIndex_, 0.0);  // finito lo spazio vuoto: il pezzo dopo
            player_->play();
        } else if (playing_ && speed_ < 0 && position_ <= gapStart + 1e-6) {
            // All'indietro: finito lo spazio vuoto, la fine del pezzo prima (o l'inizio del montaggio).
            if (gapIndex_ == 0) {
                position_ = 0.0;
                reachedEdge();
                return;
            }
            const int previous = gapIndex_ - 1;
            load(previous, sequence.fragments()[static_cast<std::size_t>(previous)].length() - 0.001);
            player_->play();
        }
    } else if (index_ >= 0 && index_ < static_cast<int>(sequence.fragments().size())) {
        const core::Fragment& f = sequence.fragments()[index_];
        position_ = sequence.startOf(index_) + std::clamp(player_->currentPosition() - f.start, 0.0, f.length());
        // Con i fotogrammi chiave la trasformazione cambia a ogni fotogramma (senza, setFrameTransform non fa nulla).
        if (!f.animation.empty() && !manipDrag_)
            player_->setFrameTransform(v1Transform(), frameSize());
        // Arrivati al tratto di una transizione, anche da un pezzo solo: da qui in poi la fusione la fa il monitor.
        if (playing_ && speed_ != 0.0) {
            if (const auto placement = transitionAt(position_))
                enterTransition(*placement, position_, false);
            else
                primeTransition();
        }
    }
    updateLayers();
    updateLabels();
    updateTextOverlay();
    if (onPositionChanged)
        onPositionChanged(position_);
    if (onCacheRangeChanged) {
        double from = 0.0;
        double to = 0.0;
        if (gapIndex_ < 0 && index_ >= 0 && index_ < static_cast<int>(sequence.fragments().size())) {
            // Solo la parte del pezzo: il resto del file non è nel montaggio.
            const core::Fragment& f = sequence.fragments()[static_cast<std::size_t>(index_)];
            const auto [cacheFrom, cacheTo] = player_->cachedRange();
            const double start = sequence.startOf(index_);
            from = start + std::clamp(cacheFrom, f.start, f.end) - f.start;
            to = start + std::clamp(cacheTo, f.start, f.end) - f.start;
        }
        onCacheRangeChanged(from, to);
    }
}

bool ProgramMonitor::startAudio(double from)
{
    stopAudio();
    const core::Sequence& sequence = project_.sequence();
    if (!hasProgram() || !core::audibleAtSpeed(speed_)) {
        player_->setAudioNote(QString());
        return false;
    }
    applyVolume();
    core::AudioMixer mixer(core::mixSegmentsOf(project_), core::trackGainsOf(project_.audioTracks()));
    const bool started = sequenceAudio_.start(std::move(mixer), from, programEnd(), speed_);
    player_->setAudioNote(started ? QString() : QStringLiteral("audio non disponibile"));
    return started;
}

void ProgramMonitor::stopAudio()
{
    sequenceAudio_.stop();
}

void ProgramMonitor::applyVolume()
{
    // Al quadrato: il cursore segue meglio come si sente il volume.
    const float level = static_cast<float>(volume_) / 100.0f;
    sequenceAudio_.setVolume(muted_ ? 0.0f : level * level);
}

void ProgramMonitor::audioMixChanged()
{
    sequenceAudio_.setTrackGains(core::trackGainsOf(project_.audioTracks()));
    sequenceAudio_.setSegmentEnvelopes(core::segmentEnvelopesOf(project_));
}

void ProgramMonitor::updateLabels()
{
    const core::Sequence& sequence = project_.sequence();
    transport_->setMediaEnabled(hasProgram());
    transport_->setPlaying(playing_);
    transport_->setSpeed(speed_);
    transport_->setLoop(loop_);
    transport_->setVolume(volume_, muted_);
    timeLabel_->setText(QStringLiteral("%1 / %2").arg(timeText(position_), timeText(programEnd())));
    if (gapIndex_ == static_cast<int>(sequence.fragments().size())) {
        fragmentLabel_->setText(QStringLiteral("Dopo l'ultimo pezzo (nero) · scritte e livelli"));
        return;
    }
    if (gapIndex_ >= 0 && gapIndex_ < static_cast<int>(sequence.fragments().size())) {
        fragmentLabel_->setText(QStringLiteral("Spazio vuoto (nero) prima del pezzo %1").arg(gapIndex_ + 1));
        return;
    }
    if (transitionId_ != 0) {
        if (const core::TransitionClip* transition = project_.transitions().find(transitionId_)) {
            const core::Transition* effect = core::findTransition(transition->type);
            QString text = QStringLiteral("Transizione %1 fra il pezzo %2 e il pezzo %3")
                               .arg(effect ? QString::fromUtf8(effect->name()) : QString::fromStdString(transition->type))
                               .arg(sequence.indexOf(transition->beforeFragment) + 1)
                               .arg(sequence.indexOf(transition->afterFragment) + 1);
            // In riproduzione, quanto ci vuole a preparare un fotogramma: sotto la durata di un fotogramma va liscia.
            const double prepare = transitionPlayback_.averageRenderSeconds();
            if (playing_ && prepare > 0.0) {
                const auto timing = transitionPlayback_.averageTiming();
                text += QStringLiteral(" · anteprima: %1 ms a fotogramma (limite %2 ms; decodifica %3 (%7) + %4 (%8), fusione %5, riposizionamenti %6)")
                            .arg(prepare * 1000.0, 0, 'f', 0)
                            .arg(player_->frameDuration() * 1000.0 / std::max(1.0, std::abs(speed_)), 0, 'f', 0)
                            .arg(timing.decodeFrom * 1000.0, 0, 'f', 0)
                            .arg(timing.decodeTo * 1000.0, 0, 'f', 0)
                            .arg(timing.blend * 1000.0, 0, 'f', 0)
                            .arg(timing.seeks)
                            .arg(timing.hardwareFrom ? QStringLiteral("GPU") : QStringLiteral("CPU"))
                            .arg(timing.hardwareTo ? QStringLiteral("GPU") : QStringLiteral("CPU"));
            }
            fragmentLabel_->setText(text);
            return;
        }
    }
    if (index_ < 0 || index_ >= static_cast<int>(sequence.fragments().size())) {
        fragmentLabel_->setText(sequence.empty() ? QStringLiteral("Sequenza vuota") : QString());
        return;
    }
    const core::Fragment& f = sequence.fragments()[index_];
    const core::Clip* clip = project_.find(f.clipId);
    fragmentLabel_->setText(QStringLiteral("Pezzo %1 di %2: %3 (%4 - %5)")
                                .arg(index_ + 1)
                                .arg(sequence.fragments().size())
                                .arg(clip ? fileNameOf(clip->path) : QString(),
                                     timeText(f.start), timeText(f.end)));
}

}  // namespace editor::app
