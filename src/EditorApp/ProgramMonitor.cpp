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

    auto* controls = new QHBoxLayout;
    controls->addWidget(playButton_);
    controls->addWidget(timeLabel_);
    controls->addSpacing(12);
    controls->addWidget(fragmentLabel_, 1);

    auto* title = new QLabel(QStringLiteral("<b>Montaggio</b> (il risultato)"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title);
    layout->addWidget(player_, 1);
    layout->addLayout(controls);

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
    if (index < 0)
        index = static_cast<int>(sequence.fragments().size()) - 1;  // alla fine: ultimo pezzo
    const bool wasPlaying = playing_;
    if (wasPlaying)
        player_->pause();
    load(index, t - sequence.startOf(index));
    if (wasPlaying)
        player_->play();
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
    player_->play();
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
    // Inizio di ogni pezzo, sommando le durate una volta sola (come startOf, nello stesso ordine).
    std::vector<double> starts(static_cast<std::size_t>(count));
    double start = 0.0;
    for (int i = 0; i < count; ++i) {
        starts[static_cast<std::size_t>(i)] = start;
        start += sequence.fragments()[static_cast<std::size_t>(i)].length();
    }
    if (direction > 0) {
        target = sequence.totalLength();
        for (int i = 1; i < count; ++i) {
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
    const int next = index_ + 1;
    if (next < static_cast<int>(project_.sequence().fragments().size())) {
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
    if (index_ >= 0 && index_ < static_cast<int>(sequence.fragments().size())) {
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
