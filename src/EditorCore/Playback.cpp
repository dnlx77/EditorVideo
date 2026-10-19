#include "EditorCore/Playback.h"

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

}  // namespace editor::core
