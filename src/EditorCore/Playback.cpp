#include "EditorCore/Playback.h"

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

}  // namespace editor::core
