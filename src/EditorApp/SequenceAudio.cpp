#include "EditorApp/SequenceAudio.h"

#include "EditorCore/Playback.h"
#include "EditorCore/TimeStretch.h"

#include <QtCore/QtGlobal>

#include <algorithm>
#include <cmath>
#include <optional>

namespace editor::app {

namespace {

// Blocchi di ~21 ms: il livello di una traccia cambia quasi subito.
constexpr std::size_t kBlockFrames = 1024;

}  // namespace

SequenceAudio::SequenceAudio() = default;

SequenceAudio::~SequenceAudio()
{
    stop();
    out_.close();
}

bool SequenceAudio::start(core::AudioMixer mixer, double from, double length, double speed)
{
    stop();
    if (!core::audibleAtSpeed(speed))
        return false;
    // EDITORVIDEO_NO_AUDIO=1 simula un PC senza scheda audio (per provare quel caso anche dove la scheda c'è).
    if (qEnvironmentVariableIntValue("EDITORVIDEO_NO_AUDIO") != 0)
        return false;
    if (!opened_)
        opened_ = out_.open(core::kMixSampleRate, core::kMixChannels);
    if (!opened_)
        return false;

    mixer_ = std::make_unique<core::AudioMixer>(std::move(mixer));
    from_ = std::max(0.0, from);
    length_ = length;
    speed_ = speed;
    out_.reset();
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] { work(); });
    return true;
}

void SequenceAudio::stop()
{
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    if (running_ && opened_)
        out_.reset();
    running_ = false;
    mixer_.reset();
}

void SequenceAudio::setTrackGains(const std::map<int, float>& gains)
{
    if (running_ && mixer_)
        mixer_->setGains(gains);
}

void SequenceAudio::setTrackEnvelopes(const core::TrackEnvelopes& envelopes)
{
    if (running_ && mixer_)
        mixer_->setEnvelopes(envelopes);
}

double SequenceAudio::position() const
{
    const double played = static_cast<double>(out_.playedFrames()) / core::kMixSampleRate;
    return std::min(length_, from_ + played * speed_);
}

void SequenceAudio::work()
{
    try {
        const auto totalFrames = static_cast<long long>(std::llround(length_ * core::kMixSampleRate));
        long long frame = static_cast<long long>(std::llround(from_ * core::kMixSampleRate));
        std::optional<core::TimeStretch> stretch;
        if (speed_ != 1.0)
            stretch.emplace(core::kMixChannels, core::kMixSampleRate, speed_);
        std::vector<std::int16_t> block;
        std::vector<std::int16_t> out;
        const auto send = [&](std::vector<std::int16_t>& samples) {
            const float volume = volume_;
            if (volume != 1.0f) {
                for (std::int16_t& v : samples)
                    v = static_cast<std::int16_t>(std::clamp(std::lround(v * volume), -32768L, 32767L));
            }
            return samples.empty() || out_.write(samples.data(), samples.size(), stop_);
        };
        // Il mix si calcola alla velocità 1x; a velocità diverse ci pensa TimeStretch (ingresso e uscita differiscono).
        while (!stop_ && frame < totalFrames) {
            const auto n = static_cast<std::size_t>(std::min<long long>(kBlockFrames, totalFrames - frame));
            mixer_->render(frame, n, block);
            frame += static_cast<long long>(n);
            if (stretch) {
                out.clear();
                stretch->push(block.data(), block.size());
                stretch->pull(out);
                if (!send(out))
                    break;
            } else if (!send(block)) {
                break;
            }
        }
        if (!stop_) {
            if (stretch) {
                out.clear();
                stretch->flush(out);
                send(out);
            }
            out_.flush();
        }
    } catch (const std::exception&) {
        // Errore nel mix: l'audio si ferma, la riproduzione prosegue a tempo reale.
    }
}

}  // namespace editor::app
