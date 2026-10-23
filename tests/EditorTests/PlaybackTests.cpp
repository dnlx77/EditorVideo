#include "EditorCore/Playback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

using editor::core::PlaybackState;
using editor::core::playbackFinished;
using editor::core::writeSilence;

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

TEST(Playback, LongSilenceIsWrittenInSmallBlocks)
{
    // Un'ora di silenzio stereo a 48 kHz: tutti i campioni arrivano, a blocchi di 100 ms, e sono zeri.
    const std::size_t frames = 48000ull * 3600;
    std::size_t total = 0;
    std::size_t largest = 0;
    bool allZero = true;
    const bool ok = writeSilence(frames, 2, 48000, [&](const std::int16_t* samples, std::size_t count) {
        total += count;
        largest = std::max(largest, count);
        allZero = allZero && std::all_of(samples, samples + count, [](std::int16_t v) { return v == 0; });
        return true;
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(total, frames * 2);
    EXPECT_EQ(largest, 4800u * 2);
    EXPECT_TRUE(allZero);
}

TEST(Playback, SilenceStopsWhenAsked)
{
    int calls = 0;
    const bool ok = writeSilence(48000 * 10, 2, 48000, [&](const std::int16_t*, std::size_t) { return ++calls < 3; });
    EXPECT_FALSE(ok);
    EXPECT_EQ(calls, 3);
}

TEST(Playback, ShortSilenceIsOneBlock)
{
    std::size_t total = 0;
    int calls = 0;
    EXPECT_TRUE(writeSilence(100, 1, 44100, [&](const std::int16_t*, std::size_t count) {
        total += count;
        ++calls;
        return true;
    }));
    EXPECT_EQ(total, 100u);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(writeSilence(0, 2, 48000, [](const std::int16_t*, std::size_t) { return false; }));
}

}  // namespace

namespace {

using editor::core::audibleAtSpeed;
using editor::core::shuttleSpeed;

TEST(Playback, ShuttleStartsAtNormalSpeedThenDoubles)
{
    EXPECT_EQ(shuttleSpeed(1.0, false, +1), 1.0);   // fermo: L parte a 1x
    EXPECT_EQ(shuttleSpeed(1.0, true, +1), 2.0);    // L ripetuto
    EXPECT_EQ(shuttleSpeed(2.0, true, +1), 4.0);
    EXPECT_EQ(shuttleSpeed(4.0, true, +1), 4.0);    // non oltre 4x
    EXPECT_EQ(shuttleSpeed(0.5, true, +1), 1.0);    // dal rallentatore: 1x
    EXPECT_EQ(shuttleSpeed(1.0, false, -1), -1.0);  // J: indietro a 1x
    EXPECT_EQ(shuttleSpeed(-1.0, true, -1), -2.0);
    EXPECT_EQ(shuttleSpeed(-4.0, true, +1), 1.0);   // il tasto opposto riparte a 1x
    EXPECT_EQ(shuttleSpeed(2.0, true, -1), -1.0);
}

TEST(Playback, AudioOnlyForwardUpToDoubleSpeed)
{
    EXPECT_TRUE(audibleAtSpeed(0.25));
    EXPECT_TRUE(audibleAtSpeed(1.0));
    EXPECT_TRUE(audibleAtSpeed(2.0));
    EXPECT_FALSE(audibleAtSpeed(4.0));
    EXPECT_FALSE(audibleAtSpeed(-1.0));
}

}  // namespace
