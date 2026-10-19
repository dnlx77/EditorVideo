#include "EditorCore/Playback.h"

#include <gtest/gtest.h>

using editor::core::PlaybackState;
using editor::core::playbackFinished;

namespace {

TEST(Playback, VideoAndAudioMustBothFinish)
{
    PlaybackState s{.hasVideo = true, .audioActive = true, .clock = 3.0, .duration = 10.0};
    EXPECT_FALSE(playbackFinished(s));
    s.videoFinished = true;
    EXPECT_FALSE(playbackFinished(s));  // l'audio continua
    s.audioFinished = true;
    EXPECT_TRUE(playbackFinished(s));
}

TEST(Playback, VideoWithoutAudioEndsWithLastFrame)
{
    // Video muto, o scheda audio assente: decide il video, anche prima della durata dichiarata.
    PlaybackState s{.hasVideo = true, .audioActive = false, .clock = 9.9, .duration = 10.0};
    EXPECT_FALSE(playbackFinished(s));
    s.videoFinished = true;
    EXPECT_TRUE(playbackFinished(s));
}

TEST(Playback, AudioOnlyEndsWhenSoundCardHasPlayedEverything)
{
    PlaybackState s{.hasVideo = false, .audioActive = true, .clock = 4.0, .duration = 10.0};
    EXPECT_FALSE(playbackFinished(s));
    s.audioFinished = true;
    EXPECT_TRUE(playbackFinished(s));
}

TEST(Playback, AudioOnlyWithoutSoundCardFollowsTheClock)
{
    // Il caso del bug: niente video e niente scheda audio. Non deve finire subito ma alla durata del file.
    PlaybackState s{.hasVideo = false, .audioActive = false, .clock = 0.0, .duration = 10.0};
    EXPECT_FALSE(playbackFinished(s));
    s.clock = 2.0;
    EXPECT_FALSE(playbackFinished(s));
    s.clock = 9.999;
    EXPECT_FALSE(playbackFinished(s));
    s.clock = 10.0;
    EXPECT_TRUE(playbackFinished(s));
}

TEST(Playback, NothingToPlayAndUnknownDurationStops)
{
    // Senza durata non c'è un punto d'arrivo: meglio fermarsi che andare avanti per sempre.
    PlaybackState s{.hasVideo = false, .audioActive = false, .clock = 0.0, .duration = 0.0};
    EXPECT_TRUE(playbackFinished(s));
}

}  // namespace
