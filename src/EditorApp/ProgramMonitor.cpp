#include "EditorApp/ProgramMonitor.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/TransportBar.h"
#include "EditorApp/UiText.h"
#include "EditorCore/Playback.h"
#include "EditorCore/Project.h"

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace editor::app {

ProgramMonitor::ProgramMonitor(const core::Project& project, QWidget* parent)
    : QWidget(parent)
    , project_(project)
{
    player_ = new PlayerWidget;
    player_->setChromeVisible(false);
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
    layout->addWidget(fragmentLabel_);

    poll_ = new QTimer(this);
    poll_->setInterval(40);
    connect(poll_, &QTimer::timeout, this, [this] { updatePosition(); });

    updateLabels();
}

void ProgramMonitor::mousePressEvent(QMouseEvent* event)
{
    if (onActivated)
        onActivated();
    QWidget::mousePressEvent(event);
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
    player_->setBlank(false);
    if (f.clipId != loadedClipId_) {
        player_->openFile(QString::fromStdString(clip->path));
        loadedClipId_ = f.clipId;
    }
    index_ = index;
    // Audio del pezzo: i suoi tratti silenziati e, se c'è, la traccia sostitutiva (al tempo t del file si sente
    // il punto della traccia che cade lì nella sequenza).
    player_->setAudioMutes(f.muted);
    if (const auto& replacement = project_.audioReplacement())
        player_->setExternalAudio(QString::fromStdString(replacement->path),
                                  project_.sequence().startOf(index) - f.start - replacement->offset);
    else
        player_->setExternalAudio(QString(), 0.0);
    player_->setPlaybackWindow(f.start, f.end);
    player_->seekTo(f.start + std::clamp(offset, 0.0, f.length()));
}

void ProgramMonitor::seekTo(double seconds)
{
    const core::Sequence& sequence = project_.sequence();
    if (sequence.empty())
        return;
    const double t = std::clamp(seconds, 0.0, sequence.totalLength());
    int index = sequence.indexAt(t);
    if (index < 0) {
        if (const int gap = sequence.gapIndexAt(t); gap >= 0) {
            enterGap(gap, t);
            return;
        }
        index = static_cast<int>(sequence.fragments().size()) - 1;  // alla fine: ultimo pezzo
    }
    const bool wasPlaying = playing_;
    if (wasPlaying)
        player_->pause();
    load(index, t - sequence.startOf(index));
    if (wasPlaying)
        player_->play();
    updatePosition();
}

void ProgramMonitor::enterGap(int index, double seconds)
{
    if (player_->isPlaying())
        player_->pause();
    player_->setBlank(true);
    index_ = -1;
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
    if (sequence.empty())
        return;
    if (playing_) {
        updatePosition();  // nello spazio vuoto: il tempo arrivato fin qui
        player_->pause();
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
    if (sequence.empty() || playing_)
        return;
    if (speed_ < 0) {
        if (position_ <= 0.0005) {
            setCurrentSpeed(baseSpeed_);  // all'indietro dall'inizio: niente da riprodurre
            return;
        }
        if (index_ < 0 && gapIndex_ < 0)
            seekTo(position_);
    } else if (position_ >= sequence.totalLength() - 0.05) {
        seekTo(0.0);  // alla fine: si ricomincia
    } else if (index_ < 0) {
        seekTo(position_);
    }
    playing_ = true;
    if (onPlaybackStarted)
        onPlaybackStarted();
    if (gapIndex_ >= 0) {
        gapBase_ = position_;  // nello spazio vuoto scorre solo il tempo
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
    if (project_.sequence().empty())
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
    if (gapIndex_ >= 0 && playing_) {
        updatePosition();  // nello spazio vuoto il tempo riparte da qui con la nuova velocità
        gapBase_ = position_;
        gapClock_.restart();
    }
    speed_ = speed;
    player_->setSpeed(speed);
    updateLabels();
}

void ProgramMonitor::shuttle(int direction)
{
    if (project_.sequence().empty())
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
    player_->setVolume(volume_);
    player_->setMuted(muted_);
    updateLabels();
}

void ProgramMonitor::setMuted(bool muted)
{
    muted_ = muted;
    player_->setMuted(muted);
    updateLabels();
}

void ProgramMonitor::stepFrame(int direction)
{
    if (project_.sequence().empty())
        return;
    setPlaying(false);
    // Un fotogramma del file mostrato; a cavallo di una giunzione seekTo() passa al pezzo giusto.
    seekTo(position_ + direction * player_->frameDuration() + 0.0005);
}

void ProgramMonitor::stepToCut(int direction)
{
    const core::Sequence& sequence = project_.sequence();
    if (sequence.empty())
        return;
    setPlaying(false);
    const std::vector<double> starts = sequence.starts();
    const int count = static_cast<int>(starts.size());
    double target = position_;
    if (direction > 0) {
        target = sequence.totalLength();
        for (int i = 0; i < count; ++i) {
            if (starts[static_cast<std::size_t>(i)] > position_ + 0.001) {
                target = starts[static_cast<std::size_t>(i)];
                break;
            }
        }
        if (target >= sequence.totalLength())
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
    if (sequence.empty())
        return;
    setPlaying(false);
    seekTo(std::max(0.0, sequence.totalLength() - player_->frameDuration()));
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
    } else {
        return reachedEdge();
    }
    updatePosition();
}

void ProgramMonitor::reachedEdge()
{
    const core::Sequence& sequence = project_.sequence();
    if (loop_ && !sequence.empty()) {
        // Ripetizione: si riparte dall'altro capo del montaggio, senza fermarsi.
        seekTo(speed_ < 0 ? std::max(0.0, sequence.totalLength() - player_->frameDuration()) : 0.0);
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
    poll_->stop();
    if (speed_ < 0)
        seekTo(0.0);
    setCurrentSpeed(baseSpeed_);
    updatePosition();
}

void ProgramMonitor::sequenceChanged()
{
    const core::Sequence& sequence = project_.sequence();
    if (playing_) {
        player_->pause();
        playing_ = false;
        poll_->stop();
    }
    setCurrentSpeed(baseSpeed_);
    if (loadedClipId_ != 0 && project_.find(loadedClipId_) == nullptr)
        loadedClipId_ = 0;
    if (sequence.empty()) {
        index_ = -1;
        gapIndex_ = -1;
        player_->setBlank(false);
        position_ = 0.0;
        loadedClipId_ = 0;
        player_->closeFile();
        updatePosition();
        return;
    }
    seekTo(std::min(position_, sequence.totalLength()));
}

void ProgramMonitor::updatePosition()
{
    const core::Sequence& sequence = project_.sequence();
    if (gapIndex_ >= 0 && gapIndex_ < static_cast<int>(sequence.fragments().size())) {
        const double gapEnd = sequence.startOf(gapIndex_);
        const double gapStart = gapEnd - sequence.fragments()[static_cast<std::size_t>(gapIndex_)].gap;
        if (playing_)
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
    }
    updateLabels();
    if (onPositionChanged)
        onPositionChanged(position_);
}

void ProgramMonitor::updateLabels()
{
    const core::Sequence& sequence = project_.sequence();
    transport_->setMediaEnabled(!sequence.empty());
    transport_->setPlaying(playing_);
    transport_->setSpeed(speed_);
    transport_->setLoop(loop_);
    transport_->setVolume(volume_, muted_);
    timeLabel_->setText(QStringLiteral("%1 / %2").arg(timeText(position_), timeText(sequence.totalLength())));
    if (gapIndex_ >= 0 && gapIndex_ < static_cast<int>(sequence.fragments().size())) {
        fragmentLabel_->setText(QStringLiteral("Spazio vuoto (nero) prima del pezzo %1").arg(gapIndex_ + 1));
        return;
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
