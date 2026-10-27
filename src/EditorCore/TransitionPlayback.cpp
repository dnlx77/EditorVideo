#include "EditorCore/TransitionPlayback.h"

#include <chrono>

namespace editor::core {

TransitionPlayback::TransitionPlayback()
{
    thread_ = std::thread([this] { run(); });
}

TransitionPlayback::~TransitionPlayback()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    done_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void TransitionPlayback::setBox(int width, int height)
{
    std::lock_guard lock(mutex_);
    if (width == boxWidth_ && height == boxHeight_)
        return;
    boxWidth_ = width;
    boxHeight_ = height;
    configChanged_ = true;
}

void TransitionPlayback::setVideoSource(std::function<std::string(const std::string& path)> source)
{
    std::lock_guard lock(mutex_);
    source_ = std::move(source);
    sourceChanged_ = true;
}

void TransitionPlayback::setHardwareAllowed(bool allowed)
{
    std::lock_guard lock(mutex_);
    if (allowed != hardware_) {
        hardware_ = allowed;
        configChanged_ = true;
    }
}

void TransitionPlayback::reset()
{
    std::lock_guard lock(mutex_);
    resetRequested_ = true;
}

unsigned long long TransitionPlayback::request(const TransitionJob& job, double sequenceTime)
{
    unsigned long long serial = 0;
    {
        std::lock_guard lock(mutex_);
        serial = ++serial_;
        request_ = {job, sequenceTime, serial};
        hasRequest_ = true;
    }
    wake_.notify_one();
    return serial;
}

std::shared_ptr<VideoFrame> TransitionPlayback::render(const TransitionJob& job, double sequenceTime, int timeoutMs)
{
    const unsigned long long serial = request(job, sequenceTime);
    std::unique_lock lock(mutex_);
    done_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return stop_ || result_.serial >= serial; });
    return result_.serial == serial ? result_.frame : nullptr;
}

TransitionPlayback::Result TransitionPlayback::latest() const
{
    std::lock_guard lock(mutex_);
    return result_;
}

double TransitionPlayback::averageRenderSeconds() const
{
    std::lock_guard lock(mutex_);
    return averageSeconds_;
}

void TransitionPlayback::run()
{
    TransitionRenderer renderer;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || hasRequest_; });
        if (stop_)
            return;
        Request job = std::move(request_);
        hasRequest_ = false;
        // Le impostazioni cambiate dopo l'ultima richiesta.
        if (sourceChanged_) {
            renderer.setVideoSource(source_);
            sourceChanged_ = false;
        }
        if (configChanged_) {
            renderer.setHardwareAllowed(hardware_);
            renderer.setBox(boxWidth_, boxHeight_);
            configChanged_ = false;
        }
        if (resetRequested_) {
            renderer.reset();
            resetRequested_ = false;
        }
        lock.unlock();
        const auto begin = std::chrono::steady_clock::now();
        std::shared_ptr<VideoFrame> frame = renderer.render(job.job, job.time);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        lock.lock();
        result_ = {std::move(frame), job.time, job.serial};
        averageSeconds_ = averageSeconds_ == 0.0 ? seconds : 0.8 * averageSeconds_ + 0.2 * seconds;
        done_.notify_all();
    }
}

}  // namespace editor::core
