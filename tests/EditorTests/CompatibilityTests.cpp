#include "EditorCore/Compatibility.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

// Un video tipico da telefono: H.264 1920x1080 30 fps + AAC 48 kHz stereo.
MediaSignature phoneVideo(const std::string& name)
{
    MediaSignature m;
    m.path = "C:\\Video\\" + name;
    m.fileName = name;
    StreamSignature video;
    video.kind = StreamKind::Video;
    video.codec = "h264";
    video.profile = "High";
    video.width = 1920;
    video.height = 1080;
    video.pixelFormat = "yuv420p";
    video.frameRate = 30.0;
    video.extradata = {1, 100, 0, 40, 0xff, 0xe1};
    StreamSignature audio;
    audio.kind = StreamKind::Audio;
    audio.codec = "aac";
    audio.profile = "LC";
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.channelLayout = "stereo";
    audio.extradata = {0x11, 0x90};
    m.streams = {video, audio};
    return m;
}

TEST(Compatibility, SameParametersAreCompatible)
{
    EXPECT_TRUE(checkCompatibility({phoneVideo("a.mp4"), phoneVideo("b.mp4"), phoneVideo("c.mov")}).empty());
    EXPECT_TRUE(checkCompatibility({phoneVideo("solo.mp4")}).empty());
    EXPECT_TRUE(checkCompatibility({}).empty());
}

TEST(Compatibility, DifferentResolutionNamesClipAndValues)
{
    MediaSignature small = phoneVideo("nome.mp4");
    small.streams[0].width = 1280;
    small.streams[0].height = 720;
    const auto result = checkCompatibility({phoneVideo("a.mp4"), phoneVideo("b.mp4"), small});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].clipIndex, 2);
    // La differenza delle misure cambia anche i parametri di codifica: si segnala solo la causa.
    EXPECT_EQ(result[0].message, "Clip 3 (nome.mp4): risoluzione 1280x720 invece di 1920x1080");
}

TEST(Compatibility, DifferentCodecHidesOtherDetails)
{
    MediaSignature hevc = phoneVideo("hevc.mp4");
    hevc.streams[0].codec = "hevc";
    hevc.streams[0].profile = "Main";
    hevc.streams[0].width = 3840;
    const auto result = checkCompatibility({phoneVideo("a.mp4"), hevc});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].message, "Clip 2 (hevc.mp4): codec video hevc invece di h264");
}

TEST(Compatibility, VideoDetails)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].profile = "Main";
    other.streams[0].pixelFormat = "yuv420p10le";
    other.streams[0].frameRate = 60.0;
    const auto result = checkCompatibility({phoneVideo("a.mp4"), other});
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].message, "Clip 2 (x.mp4): profilo video Main invece di High");
    EXPECT_EQ(result[1].message, "Clip 2 (x.mp4): formato pixel yuv420p10le invece di yuv420p");
    EXPECT_EQ(result[2].message, "Clip 2 (x.mp4): frequenza fotogrammi 60 fps invece di 30 fps");
}

TEST(Compatibility, NearlyEqualFrameRatesAreTheSame)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].frameRate = 30.0001;
    EXPECT_TRUE(checkCompatibility({phoneVideo("a.mp4"), other}).empty());
}

TEST(Compatibility, AudioDetails)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[1].sampleRate = 44100;
    other.streams[1].channels = 1;
    other.streams[1].channelLayout = "mono";
    const auto result = checkCompatibility({phoneVideo("a.mp4"), other});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].message, "Clip 2 (x.mp4): frequenza di campionamento 44100 Hz invece di 48000 Hz");
    EXPECT_EQ(result[1].message, "Clip 2 (x.mp4): canali 1 (mono) invece di 2 (stereo)");
}

TEST(Compatibility, DifferentExtradataIsRefused)
{
    // Stessi codec e misure ma SPS/PPS diversi (es. impostazioni dell'encoder cambiate fra due riprese).
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].extradata.back() = 0x00;
    other.streams[1].extradata = {0x12, 0x10};
    const auto result = checkCompatibility({phoneVideo("a.mp4"), other});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].message, "Clip 2 (x.mp4): parametri di codifica video diversi (SPS/PPS)");
    EXPECT_EQ(result[1].message, "Clip 2 (x.mp4): parametri di codifica audio diversi");
}

TEST(Compatibility, DifferentStreamLayout)
{
    MediaSignature mute = phoneVideo("muto.mp4");
    mute.streams.pop_back();
    const auto result = checkCompatibility({phoneVideo("a.mp4"), mute});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].message, "Clip 2 (muto.mp4): 1 flussi video e 0 audio invece di 1 video e 1 audio");
}

TEST(Compatibility, EveryClipIsComparedWithTheFirst)
{
    MediaSignature b = phoneVideo("b.mp4");
    b.streams[1].sampleRate = 44100;
    MediaSignature c = phoneVideo("c.mp4");
    c.streams[0].width = 1280;
    const auto result = checkCompatibility({phoneVideo("a.mp4"), b, c});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].clipIndex, 1);
    EXPECT_EQ(result[1].clipIndex, 2);
    EXPECT_EQ(describeMismatches(result),
              "Clip 2 (b.mp4): frequenza di campionamento 44100 Hz invece di 48000 Hz\n"
              "Clip 3 (c.mp4): risoluzione 1280x1080 invece di 1920x1080");
}

}  // namespace
