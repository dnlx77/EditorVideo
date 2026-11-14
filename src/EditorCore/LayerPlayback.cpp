#include "EditorCore/LayerPlayback.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace editor::core {

LayerPlayback::LayerPlayback(std::string utf8Path, int boxWidth, int boxHeight, bool allowHardware)
    : path_(std::move(utf8Path))
    , hardware_(allowHardware)
    , boxWidth_(boxWidth)
    , boxHeight_(boxHeight)
{
    thread_ = std::thread([this] { run(); });
}

LayerPlayback::~LayerPlayback()
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

void LayerPlayback::setBox(int width, int height)
{
    std::lock_guard lock(mutex_);
    if (width == boxWidth_ && height == boxHeight_)
        return;
    boxWidth_ = width;
    boxHeight_ = height;
    boxChanged_ = true;
}

void LayerPlayback::request(double fileTime, bool playing)
{
    {
        std::lock_guard lock(mutex_);
        playing_ = playing;
        if (lastRequested_ >= 0.0 && std::abs(fileTime - lastRequested_) > 1e-6)
            backward_ = fileTime < lastRequested_;  // (un pezzo inverso chiede tempi sempre più piccoli)
        lastRequested_ = fileTime;
        bool seek = false;
        if (!playing) {
            // Il fotogramma giusto è quello che inizia prima di `fileTime` e finisce dopo.
            if (queue_.empty()) {
                seek = true;
            } else {
                const double from = queue_.front()->time;
                seek = !(fileTime >= from - 1e-6 && fileTime < from + frameDuration_ - 1e-6);
            }
        } else if (lastDecoded_ < 0.0) {
            seek = true;
        } else {
            // In riproduzione si sta dietro la decodifica, tranne se il tempo è tornato indietro o è molto più avanti.
            const double newest = queue_.empty() ? lastDecoded_ : queue_.back()->time;
            const double oldest = queue_.empty() ? lastDecoded_ : queue_.front()->time;
            seek = fileTime < oldest - 0.25 || fileTime > newest + 1.0;
        }
        if (seek) {
            hasSeek_ = true;
            seekTarget_ = fileTime;
            ++seekSerial_;
            queue_.clear();
            ended_ = false;
        }
    }
    wake_.notify_one();
}

std::shared_ptr<VideoFrame> LayerPlayback::frame(double fileTime)
{
    std::lock_guard lock(mutex_);
    while (queue_.size() >= 2 && queue_[1]->time <= fileTime + 1e-6)
        queue_.pop_front();
    if (queue_.empty())
        return nullptr;
    // In riproduzione un fotogramma che deve ancora arrivare (più di un fotogramma e mezzo dopo) non si mostra.
    if (playing_ && queue_.front()->time > fileTime + frameDuration_ * 1.5)
        return nullptr;
    const std::shared_ptr<VideoFrame> shown = queue_.front();
    wake_.notify_one();  // c'è di nuovo posto in coda
    return shown;
}

std::shared_ptr<VideoFrame> LayerPlayback::waitFrame(double fileTime, int timeoutMs)
{
    request(fileTime, false);
    std::unique_lock lock(mutex_);
    const auto ready = [&] {
        return stop_ || failed_.load()
               || (!queue_.empty() && fileTime >= queue_.front()->time - 1e-6 && fileTime < queue_.front()->time + frameDuration_ + 1e-6)
               || (!queue_.empty() && !hasSeek_ && !decoding_);
    };
    done_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
    return queue_.empty() ? nullptr : queue_.front();
}

void LayerPlayback::run()
{
    std::unique_ptr<VideoDecoder> decoder;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] {
            return stop_ || hasSeek_ || boxChanged_
                   || (playing_ && !ended_ && !failed_.load() && !queue_.empty() && queue_.size() < kMaxQueued);
        });
        if (stop_)
            return;
        if (failed_.load()) {
            hasSeek_ = false;
            boxChanged_ = false;
            done_.notify_all();
            continue;
        }
        if (!decoder) {
            const int w = boxWidth_;
            const int h = boxHeight_;
            lock.unlock();
            try {
                VideoDecoder::Options options;
                options.allowHardware = hardware_;
                options.maxWidth = w;
                options.maxHeight = h;
                decoder = std::make_unique<VideoDecoder>(path_, options);
            } catch (const std::exception&) {
                decoder.reset();
            }
            lock.lock();
            if (!decoder) {
                failed_ = true;
                done_.notify_all();
                continue;
            }
            if (decoder->frameRate() > 0.0)
                frameDuration_ = 1.0 / decoder->frameRate();
            boxChanged_ = false;
            continue;
        }
        if (boxChanged_) {
            boxChanged_ = false;
            decoder->setOutputSize(boxWidth_, boxHeight_);
        }
        if (hasSeek_) {
            const double target = seekTarget_;
            const unsigned long long serial = seekSerial_;
            hasSeek_ = false;
            decoding_ = true;
            lock.unlock();
            std::shared_ptr<VideoFrame> found;
            try {
                decoder->setReadBackward(backward_.load());
                found = decoder->seek(target);
            } catch (const std::exception&) {
            }
            lock.lock();
            decoding_ = false;
            if (serial == seekSerial_ && found) {
                queue_.push_back(found);
                lastDecoded_ = found->time;
            }
            done_.notify_all();
            continue;
        }
        if (playing_ && !ended_ && !queue_.empty() && queue_.size() < kMaxQueued) {
            const unsigned long long serial = seekSerial_;
            decoding_ = true;
            lock.unlock();
            std::shared_ptr<VideoFrame> next;
            try {
                next = decoder->nextFrame();
            } catch (const std::exception&) {
            }
            lock.lock();
            decoding_ = false;
            if (serial == seekSerial_) {
                if (next) {
                    queue_.push_back(next);
                    lastDecoded_ = next->time;
                } else {
                    ended_ = true;
                }
            }
            done_.notify_all();
        }
    }
}

}  // namespace editor::core
