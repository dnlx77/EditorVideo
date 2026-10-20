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
    if (const double duration = decoder->durationSeconds(); duration > 0)
        waveform.peaks.reserve(static_cast<std::size_t>(duration / waveform.binSeconds) + 2);
    try {
        while (auto chunk = decoder->next()) {
            if (cancel != nullptr && cancel->load())
                return {};
            const std::size_t frames = chunk->samples.size() / channels;
            // Cella del campione i: il tempo si calcola come sempre, ma solo dove la cella può cambiare.
            const auto binOf = [&](std::size_t i) {
                return static_cast<std::size_t>((chunk->time + i / rate) / waveform.binSeconds);
            };
            std::size_t i = 0;
            while (i < frames && chunk->time + i / rate < 0)
                ++i;
            while (i < frames) {
                const std::size_t bin = binOf(i);
                // Primo campione della cella successiva: stima, poi correzione con lo stesso calcolo.
                const double nextStart = ((bin + 1) * waveform.binSeconds - chunk->time) * rate;
                std::size_t end = std::clamp(static_cast<std::size_t>(std::max(0.0, std::ceil(nextStart))), i + 1, frames);
                while (end > i + 1 && binOf(end - 1) != bin)
                    --end;
                while (end < frames && binOf(end) == bin)
                    ++end;

                int value = 0;
                for (std::size_t k = i * channels; k < end * channels; ++k)
                    value = std::max(value, std::abs(static_cast<int>(chunk->samples[k])));
                if (bin >= waveform.peaks.size())
                    waveform.peaks.resize(bin + 1, 0);
                const auto level = static_cast<std::uint8_t>(std::min(255, value * 255 / 32767));
                waveform.peaks[bin] = std::max(waveform.peaks[bin], level);
                i = end;
            }
        }
    } catch (const std::exception&) {
        // Audio danneggiato a metà: si tiene quello che si è letto.
    }
    return waveform;
}

}  // namespace editor::core
