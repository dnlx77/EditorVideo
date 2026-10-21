#include "EditorApp/ProgramMonitor.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/UiText.h"
#include "EditorCore/Project.h"

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
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

    playButton_ = new QToolButton;
    playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playButton_->setToolTip(QStringLiteral("Riproduci il montaggio"));
    playButton_->setFocusPolicy(Qt::NoFocus);
    connect(playButton_, &QToolButton::clicked, this, [this] {
        if (onActivated)
            onActivated();
        togglePlayPause();
    });
    timeLabel_ = new QLabel;
    fragmentLabel_ = new QLabel;
    // Le scritte non impongono una larghezza minima: in un pannello stretto si tagliano.
    timeLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fragmentLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* controls = new QHBoxLayout;
    controls->addWidget(playButton_);
    controls->addWidget(timeLabel_, 2);
    controls->addSpacing(12);
    controls->addWidget(fragmentLabel_, 5);

    title_ = new QLabel(QStringLiteral("<b>Montaggio</b> (il risultato)"));
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSizeConstraint(QLayout::SetNoConstraint);  // la misura minima la decide minimumSizeHint
    layout->addWidget(title_);
    layout->addWidget(player_, 1);
    layout->addLayout(controls);

    poll_ = new QTimer(this);
    poll_->setInterval(40);
    connect(poll_, &QTimer::timeout, this, [this] { updatePosition(); });

    updateLabels();
}

void ProgramMonitor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    title_->setVisible(height() >= 150);  // in poco spazio basta il titolo del pannello
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
    player_->setPlaybackWindow(f.end);
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
        updatePosition();
        return;
    }
    if (position_ >= sequence.totalLength() - 0.05)
        seekTo(0.0);  // alla fine: si ricomincia
    else if (index_ < 0)
        seekTo(position_);
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
    const int count = static_cast<int>(sequence.fragments().size());
    double target = position_;
    // Inizio di ogni pezzo, sommando le durate (e gli spazi vuoti) una volta sola, come startOf.
    std::vector<double> starts(static_cast<std::size_t>(count));
    double start = 0.0;
    for (int i = 0; i < count; ++i) {
        start += sequence.fragments()[static_cast<std::size_t>(i)].gap;
        starts[static_cast<std::size_t>(i)] = start;
        start += sequence.fragments()[static_cast<std::size_t>(i)].length();
    }
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
    const int next = index_ + 1;
    if (next < static_cast<int>(sequence.fragments().size())) {
        const double gap = sequence.fragments()[static_cast<std::size_t>(next)].gap;
        if (gap > 0.0) {
            enterGap(next, sequence.startOf(next) - gap);  // prima del pezzo dopo c'è uno spazio vuoto
            return;
        }
        load(next, 0.0);
        player_->play();
    } else {
        playing_ = false;  // fine del montaggio
        poll_->stop();
    }
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
        if (playing_)
            position_ = std::min(gapEnd, gapBase_ + static_cast<double>(gapClock_.nsecsElapsed()) / 1e9);
        if (playing_ && position_ >= gapEnd - 1e-6) {
            load(gapIndex_, 0.0);  // finito lo spazio vuoto: il pezzo dopo
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
    playButton_->setEnabled(!sequence.empty());
    playButton_->setIcon(style()->standardIcon(playing_ ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
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
