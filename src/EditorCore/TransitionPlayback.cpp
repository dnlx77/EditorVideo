#include "EditorCore/TransitionPlayback.h"

#include <algorithm>
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
        streamActive_ = false;
        ++streamGeneration_;
        queue_.clear();
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

void TransitionPlayback::startStream(const TransitionJob& job, double from, double step, int direction)
{
    {
        std::lock_guard lock(mutex_);
        streamJob_ = job;
        streamNext_ = std::clamp(from, job.start, job.end);
        streamStep_ = std::max(step, 1e-3);
        streamDirection_ = direction < 0 ? -1 : 1;
        clockHint_ = streamNext_;
        streamActive_ = true;
        ++streamGeneration_;
        queue_.clear();
    }
    wake_.notify_one();
}

std::shared_ptr<VideoFrame> TransitionPlayback::takeFrame(double time)
{
    std::shared_ptr<VideoFrame> frame;
    bool freed = false;
    {
        std::lock_guard lock(mutex_);
        clockHint_ = time;
        const double epsilon = 1e-4;
        const auto reached = [&](const VideoFrame& f) { return streamDirection_ > 0 ? f.time <= time + epsilon : f.time >= time - epsilon; };
        // Si scartano i fotogrammi superati: il primo resta finché il successivo non è arrivato al suo tempo.
        while (queue_.size() >= 2 && reached(*queue_[1])) {
            queue_.pop_front();
            freed = true;
        }
        if (!queue_.empty() && reached(*queue_.front()))
            frame = queue_.front();
    }
    if (freed)
        wake_.notify_one();
    return frame;
}

bool TransitionPlayback::streamWorkAvailable() const
{
    return streamActive_ && queue_.size() < kMaxQueued;
}

void TransitionPlayback::run()
{
    TransitionRenderer renderer;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || hasRequest_ || streamWorkAvailable(); });
        if (stop_)
            return;
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
        if (!hasRequest_) {
            // Un fotogramma del flusso. Se la riproduzione è più avanti del tempo da preparare, si salta.
            const TransitionJob job = streamJob_;
            const unsigned long long generation = streamGeneration_;
            double t = streamNext_;
            if (streamDirection_ > 0 ? clockHint_ > t + 2 * streamStep_ : clockHint_ < t - 2 * streamStep_)
                t = clockHint_;
            lock.unlock();
            const auto begin = std::chrono::steady_clock::now();
            std::shared_ptr<VideoFrame> frame = renderer.render(job, t);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            lock.lock();
            averageSeconds_ = averageSeconds_ == 0.0 ? seconds : 0.8 * averageSeconds_ + 0.2 * seconds;
            if (generation == streamGeneration_ && streamActive_) {
                if (!frame) {
                    streamActive_ = false;  // non si riesce a farne: il flusso finisce
                } else {
                    queue_.push_back(std::move(frame));
                    const bool last = streamDirection_ > 0 ? t >= job.end - 1e-9 : t <= job.start + 1e-9;
                    if (last)
                        streamActive_ = false;
                    else
                        streamNext_ = streamDirection_ > 0 ? std::min(t + streamStep_, job.end) : std::max(t - streamStep_, job.start);
                }
            }
            continue;
        }
        Request job = std::move(request_);
        hasRequest_ = false;
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
