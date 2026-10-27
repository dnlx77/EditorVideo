#include "EditorApp/TransitionPreviewWidget.h"

#include "EditorApp/TransitionPreview.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

constexpr double kHoldFrom = 0.6;    // secondi con A ferma
constexpr double kTransition = 1.2;  // secondi di transizione
constexpr double kHoldTo = 0.6;      // secondi con B ferma
constexpr QSize kSampleSize(480, 270);
constexpr int kNameHeight = 28;

}  // namespace

TransitionPreviewWidget::TransitionPreviewWidget(QWidget* parent) : QWidget(parent), sampleSize_(kSampleSize)
{
    setMinimumSize(80, 60);
    timer_ = new QTimer(this);
    timer_->setInterval(33);
    connect(timer_, &QTimer::timeout, this, [this] {
        setTime(std::fmod(static_cast<double>(QDateTime::currentMSecsSinceEpoch() - startedAt_) / 1000.0, cycleSeconds()));
    });
}

double TransitionPreviewWidget::cycleSeconds()
{
    return kHoldFrom + kTransition + kHoldTo;
}

double TransitionPreviewWidget::progressAt(double seconds)
{
    const double t = std::fmod(std::max(seconds, 0.0), cycleSeconds());
    return std::clamp((t - kHoldFrom) / kTransition, 0.0, 1.0);
}

void TransitionPreviewWidget::rebuildSamples()
{
    from_ = transitionSampleFrom(sampleSize_);
    to_ = transitionSampleTo(sampleSize_);
}

QImage TransitionPreviewWidget::frameAt(double seconds) const
{
    if (transition_ == nullptr)
        return {};
    QImage out(from_.size(), QImage::Format_RGBA8888);
    transition_->blend({from_.constBits(), from_.width(), from_.height(), static_cast<int>(from_.bytesPerLine())},
                       {to_.constBits(), to_.width(), to_.height(), static_cast<int>(to_.bytesPerLine())}, progressAt(seconds),
                       params_, {out.bits(), static_cast<int>(out.bytesPerLine())});
    return out;
}

void TransitionPreviewWidget::setTransition(const core::Transition* transition, const core::TransitionParams& params)
{
    transition_ = transition;
    params_ = params;
    if (from_.isNull())
        rebuildSamples();
    startedAt_ = QDateTime::currentMSecsSinceEpoch();
    setTime(0.0);
    updateTimer();
}

void TransitionPreviewWidget::setTime(double seconds)
{
    time_ = seconds;
    frame_ = frameAt(seconds);
    update();
}

void TransitionPreviewWidget::updateTimer()
{
    // Il ciclo gira solo se c'è una transizione e il pannello si vede.
    if (transition_ != nullptr && isVisible()) {
        if (!timer_->isActive()) {
            startedAt_ = QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(time_ * 1000.0);
            timer_->start();
        }
    } else {
        timer_->stop();
    }
}

void TransitionPreviewWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateTimer();
}

void TransitionPreviewWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateTimer();
}

void TransitionPreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (transition_ == nullptr || frame_.isNull())
        return;
    const QRect area(0, 0, width(), std::max(1, height() - kNameHeight));
    QSize shown = frame_.size();
    shown.scale(area.size(), Qt::KeepAspectRatio);
    const QRect target(QPoint(area.left() + (area.width() - shown.width()) / 2, area.top() + (area.height() - shown.height()) / 2), shown);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, frame_);
    painter.setPen(Qt::white);
    painter.drawText(QRect(0, height() - kNameHeight, width(), kNameHeight), Qt::AlignCenter,
                     QString::fromUtf8(transition_->name()));
}

}  // namespace editor::app
