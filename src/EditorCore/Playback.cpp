#include "EditorCore/Playback.h"

#include "EditorCore/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace editor::core {

bool playbackFinished(const PlaybackState& state)
{
    const bool videoDone = !state.hasVideo || state.videoFinished;
    const bool audioDone = !state.audioActive || state.audioFinished;
    if (!videoDone || !audioDone)
        return false;
    if (state.hasVideo || state.audioActive)
        return true;
    // Nessuna sorgente che scandisce il tempo: si finisce quando l'orologio arriva alla fine del file.
    return state.duration <= 0 || state.clock >= state.duration;
}

double shuttleSpeed(double current, bool playing, int direction)
{
    const double sign = direction < 0 ? -1.0 : 1.0;
    if (!playing || current * sign <= 0.0 || std::abs(current) < 1.0)
        return sign;
    return sign * std::min(4.0, std::abs(current) * 2.0);
}

bool audibleAtSpeed(double speed)
{
    return speed > 0.0 && speed <= 2.0;
}

bool writeSilence(std::size_t frames, int channels, int sampleRate,
                  const std::function<bool(const std::int16_t* samples, std::size_t count)>& write)
{
    const auto block = static_cast<std::size_t>(std::max(1.0, std::round(sampleRate / 10.0)));
    const auto perFrame = static_cast<std::size_t>(std::max(1, channels));
    const std::vector<std::int16_t> silence(block * perFrame, 0);
    for (std::size_t left = frames; left > 0;) {
        const std::size_t n = std::min(left, block);
        if (!write(silence.data(), n * perFrame))
            return false;
        left -= n;
    }
    return true;
}

AudioRatePipe::AudioRatePipe(int channels, int sampleRate, double speed)
{
    if (speed != 1.0)
        stretch_ = std::make_unique<TimeStretch>(channels, sampleRate, speed);
}

AudioRatePipe::~AudioRatePipe() = default;

bool AudioRatePipe::send(std::int16_t* samples, std::size_t count, float gain, const Write& write) const
{
    if (gain != 1.0f) {
        for (std::size_t i = 0; i < count; ++i)
            samples[i] = static_cast<std::int16_t>(std::clamp(std::lround(samples[i] * gain), -32768L, 32767L));
    }
    return count == 0 || write(samples, count);
}

bool AudioRatePipe::process(std::int16_t* samples, std::size_t count, float gain, const Write& write)
{
    if (!stretch_)
        return send(samples, count, gain, write);
    out_.clear();
    stretch_->push(samples, count);
    stretch_->pull(out_);
    return send(out_.data(), out_.size(), gain, write);
}

bool AudioRatePipe::finish(float gain, const Write& write)
{
    if (!stretch_)
        return true;
    out_.clear();
    stretch_->flush(out_);
    return send(out_.data(), out_.size(), gain, write);
}

}  // namespace editor::core
