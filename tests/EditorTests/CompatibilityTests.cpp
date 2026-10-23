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

// Come entra nel file un pezzo di `clip`, rispetto al formato del primo file (a.mp4).
ClipFitResult fitOf(const MediaSignature& clip, bool withAudio = true)
{
    return classifyClip(clip, formatOf(phoneVideo("a.mp4")), withAudio);
}

TEST(Compatibility, SameParametersAreCopied)
{
    const ClipFitResult result = fitOf(phoneVideo("b.mov"));
    EXPECT_EQ(result.fit, ClipFit::Copy);
    EXPECT_TRUE(result.reasons.empty());
}

TEST(Compatibility, DifferentResolutionNamesTheValues)
{
    MediaSignature small = phoneVideo("nome.mp4");
    small.streams[0].width = 1280;
    small.streams[0].height = 720;
    const ClipFitResult result = fitOf(small);
    EXPECT_EQ(result.fit, ClipFit::Reencode);
    // La differenza delle misure cambia anche i parametri di codifica: si segnala solo la causa.
    ASSERT_EQ(result.reasons.size(), 1u);
    EXPECT_EQ(result.reasons[0], "risoluzione 1280x720 invece di 1920x1080");
}

TEST(Compatibility, DifferentCodecHidesOtherDetails)
{
    MediaSignature hevc = phoneVideo("hevc.mp4");
    hevc.streams[0].codec = "hevc";
    hevc.streams[0].profile = "Main";
    hevc.streams[0].width = 3840;
    const ClipFitResult result = fitOf(hevc);
    EXPECT_EQ(result.fit, ClipFit::Reencode);
    ASSERT_EQ(result.reasons.size(), 1u);
    EXPECT_EQ(result.reasons[0], "codec video hevc invece di h264");
}

TEST(Compatibility, VideoDetails)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].profile = "Main";
    other.streams[0].pixelFormat = "yuv420p10le";
    other.streams[0].frameRate = 60.0;
    const ClipFitResult result = fitOf(other);
    EXPECT_EQ(result.fit, ClipFit::Reencode);
    ASSERT_EQ(result.reasons.size(), 3u);
    EXPECT_EQ(result.reasons[0], "profilo video Main invece di High");
    EXPECT_EQ(result.reasons[1], "formato pixel yuv420p10le invece di yuv420p");
    EXPECT_EQ(result.reasons[2], "frequenza fotogrammi 60 fps invece di 30 fps");
}

TEST(Compatibility, OnlyTheProfileDiffersCopiesWithItsParameters)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].profile = "Main";
    const ClipFitResult result = fitOf(other);
    EXPECT_EQ(result.fit, ClipFit::CopyWithParameters);
    ASSERT_EQ(result.reasons.size(), 1u);
    EXPECT_EQ(result.reasons[0], "profilo video Main invece di High");
}

TEST(Compatibility, NearlyEqualFrameRatesAreTheSame)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].frameRate = 30.0001;
    EXPECT_EQ(fitOf(other).fit, ClipFit::Copy);
}

TEST(Compatibility, AudioDetails)
{
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[1].sampleRate = 44100;
    other.streams[1].channels = 1;
    other.streams[1].channelLayout = "mono";
    const ClipFitResult result = fitOf(other);
    EXPECT_EQ(result.fit, ClipFit::Reencode);
    ASSERT_EQ(result.reasons.size(), 2u);
    EXPECT_EQ(result.reasons[0], "frequenza di campionamento 44100 Hz invece di 48000 Hz");
    EXPECT_EQ(result.reasons[1], "canali 1 (mono) invece di 2 (stereo)");
}

TEST(Compatibility, AudioIsIgnoredWhenItIsNotCopied)
{
    // Con una traccia audio sostitutiva l'audio dei file non si copia: le sue differenze non contano.
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[1].sampleRate = 44100;
    EXPECT_EQ(fitOf(other).fit, ClipFit::Reencode);
    EXPECT_EQ(fitOf(other, false).fit, ClipFit::Copy);
}

TEST(Compatibility, DifferentExtradataIsReencoded)
{
    // Stessi codec e misure ma SPS/PPS diversi (es. impostazioni dell'encoder cambiate fra due riprese).
    MediaSignature other = phoneVideo("x.mp4");
    other.streams[0].extradata.back() = 0x00;
    other.streams[1].extradata = {0x12, 0x10};
    const ClipFitResult result = fitOf(other);
    EXPECT_EQ(result.fit, ClipFit::Reencode);
    ASSERT_EQ(result.reasons.size(), 2u);
    // Dati H.264 non leggibili: ricodificato per prudenza; per l'audio un messaggio generico.
    EXPECT_EQ(result.reasons[0], "parametri di codifica video diversi (non leggibili)");
    EXPECT_EQ(result.reasons[1], "impostazioni dell'encoder audio diverse");
}

TEST(Compatibility, DifferentStreamLayout)
{
    MediaSignature mute = phoneVideo("muto.mp4");
    mute.streams.pop_back();
    const ClipFitResult result = fitOf(mute);
    EXPECT_EQ(result.fit, ClipFit::Reencode);
    ASSERT_EQ(result.reasons.size(), 1u);
    EXPECT_EQ(result.reasons[0], "1 flussi video e 0 audio invece di 1 video e 1 audio");
}

}  // namespace
