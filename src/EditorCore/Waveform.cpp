#include "EditorCore/Waveform.h"

#include "EditorCore/AudioDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace editor::core {

double Waveform::peak(double from, double to) const
{
    if (peaks.empty() || binSeconds <= 0)
        return 0.0;
    const auto last = static_cast<std::ptrdiff_t>(peaks.size()) - 1;
    const auto first = std::clamp(static_cast<std::ptrdiff_t>(std::floor(from / binSeconds)), std::ptrdiff_t{0}, last);
    const auto end = std::clamp(static_cast<std::ptrdiff_t>(std::ceil(to / binSeconds)), first + 1, last + 1);
    const auto highest = *std::max_element(peaks.begin() + first, peaks.begin() + end);
    return highest / 255.0;
}

Waveform computeWaveform(const std::string& utf8Path, const std::atomic<bool>* cancel)
{
    Waveform waveform;
    std::unique_ptr<AudioDecoder> decoder;
    try {
        decoder = std::make_unique<AudioDecoder>(utf8Path);
    } catch (const std::exception&) {
        return waveform;  // niente audio
    }
    const int channels = decoder->channels();
    const double rate = decoder->sampleRate();
    try {
        while (auto chunk = decoder->next()) {
            if (cancel != nullptr && cancel->load())
                return {};
            const std::size_t frames = chunk->samples.size() / channels;
            for (std::size_t i = 0; i < frames; ++i) {
                const double time = chunk->time + i / rate;
                if (time < 0)
                    continue;
                const auto bin = static_cast<std::size_t>(time / waveform.binSeconds);
                if (bin >= waveform.peaks.size())
                    waveform.peaks.resize(bin + 1, 0);
                int value = 0;
                for (int c = 0; c < channels; ++c)
                    value = std::max(value, std::abs(static_cast<int>(chunk->samples[i * channels + c])));
                const auto level = static_cast<std::uint8_t>(std::min(255, value * 255 / 32767));
                waveform.peaks[bin] = std::max(waveform.peaks[bin], level);
            }
        }
    } catch (const std::exception&) {
        // Audio danneggiato a metà: si tiene quello che si è letto.
    }
    return waveform;
}

}  // namespace editor::core
