#include "EditorCore/MediaProbe.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::SampleSpec;
using editor::tests::toUtf8;
using editor::tests::writeSampleFile;

namespace {

class MediaProbeTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!editor::tests::sampleEncodersAvailable())
            return;

        // 30 fotogrammi a 25 fps = 1,2 s, fotogrammi chiave a 0; 0,4; 0,8 s.
        writeSampleFile(dir_ / "video_audio.mkv", {.video = true, .audio = true});
        writeSampleFile(dir_ / "solo_video.mp4", {.video = true, .audio = false});
        writeSampleFile(dir_ / "solo_audio.wav", {.video = false, .audio = true});
        writeSampleFile(dir_ / u8"perché è così.mkv", {.video = true, .audio = false});
    }

    static void TearDownTestSuite()
    {
        // Con EDITORVIDEO_KEEP_SAMPLES=1 i file restano in %TEMP%\EditorVideoTests per provarli nell'applicazione.
        std::size_t length = 0;
        if (getenv_s(&length, nullptr, 0, "EDITORVIDEO_KEEP_SAMPLES") == 0 && length > 0)
            return;
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void SetUp() override
    {
        if (!editor::tests::sampleEncodersAvailable())
            GTEST_SKIP() << "FFmpeg senza encoder MPEG-4/PCM: impossibile generare i file di prova";
    }

    static std::string sample(const fs::path& name) { return toUtf8(dir_ / name); }

    static void expectThreeKeyframes(const MediaInfo& info)
    {
        ASSERT_EQ(info.keyframeTimes.size(), 3u);
        EXPECT_NEAR(info.keyframeTimes[0], 0.0, 0.001);
        EXPECT_NEAR(info.keyframeTimes[1], 0.4, 0.001);
        EXPECT_NEAR(info.keyframeTimes[2], 0.8, 0.001);
    }

    static inline fs::path dir_;
};

TEST_F(MediaProbeTest, ReadsMatroskaWithVideoAndAudio)
{
    const MediaInfo info = probeFile(sample("video_audio.mkv"));

    EXPECT_NE(info.formatName.find("matroska"), std::string::npos);
    EXPECT_NEAR(info.durationSeconds, 1.2, 0.05);
    ASSERT_EQ(info.streams.size(), 2u);

    const StreamInfo& video = info.streams[0];
    EXPECT_EQ(video.kind, StreamKind::Video);
    EXPECT_EQ(video.codec, "mpeg4");
    EXPECT_EQ(video.width, 64);
    EXPECT_EQ(video.height, 48);
    EXPECT_NEAR(video.frameRate, 25.0, 0.01);
    EXPECT_EQ(video.pixelFormat, "yuv420p");
    EXPECT_FALSE(video.attachedPicture);

    const StreamInfo& audio = info.streams[1];
    EXPECT_EQ(audio.kind, StreamKind::Audio);
    EXPECT_EQ(audio.codec, "pcm_s16le");
    EXPECT_EQ(audio.sampleRate, 48000);
    EXPECT_EQ(audio.channels, 2);
    EXPECT_EQ(audio.channelLayout, "stereo");
    EXPECT_EQ(audio.bitRate, 48000 * 2 * 16);

    EXPECT_EQ(info.keyframeStreamIndex, 0);
    expectThreeKeyframes(info);
}

TEST_F(MediaProbeTest, ReadsMp4VideoOnly)
{
    const MediaInfo info = probeFile(sample("solo_video.mp4"));

    EXPECT_NE(info.formatName.find("mp4"), std::string::npos);
    ASSERT_EQ(info.streams.size(), 1u);
    EXPECT_EQ(info.streams[0].kind, StreamKind::Video);
    EXPECT_GT(info.streams[0].bitRate, 0);
    expectThreeKeyframes(info);
}

TEST_F(MediaProbeTest, AudioOnlyFileHasNoKeyframes)
{
    const MediaInfo info = probeFile(sample("solo_audio.wav"));

    EXPECT_EQ(info.formatName, "wav");
    ASSERT_EQ(info.streams.size(), 1u);
    EXPECT_EQ(info.streams[0].kind, StreamKind::Audio);
    EXPECT_EQ(info.keyframeStreamIndex, -1);
    EXPECT_TRUE(info.keyframeTimes.empty());
    EXPECT_NEAR(info.durationSeconds, 1.2, 0.05);
}

TEST_F(MediaProbeTest, OpensPathWithAccentedCharacters)
{
    const MediaInfo info = probeFile(sample(u8"perché è così.mkv"));
    EXPECT_EQ(info.streams.size(), 1u);
    expectThreeKeyframes(info);
}

TEST_F(MediaProbeTest, MissingFileGivesItalianError)
{
    try {
        probeFile(sample("non_esiste.mp4"));
        FAIL() << "doveva lanciare ProbeError";
    } catch (const ProbeError& e) {
        EXPECT_NE(std::string(e.what()).find("non esiste"), std::string::npos) << e.what();
    }
}

TEST_F(MediaProbeTest, DirectoryIsRejected)
{
    EXPECT_THROW(probeFile(toUtf8(dir_)), ProbeError);
}

TEST_F(MediaProbeTest, NonMediaFileIsRejected)
{
    const fs::path path = dir_ / "testo.dat";
    std::ofstream(path) << "Questo è un file di testo, non un video.\n";

    try {
        probeFile(toUtf8(path));
        FAIL() << "doveva lanciare ProbeError";
    } catch (const ProbeCancelled&) {
        FAIL() << "non doveva risultare annullato";
    } catch (const ProbeError& e) {
        EXPECT_NE(std::string(e.what()).find("non è un video"), std::string::npos) << e.what();
    }
}

TEST_F(MediaProbeTest, CancelFlagStopsAnalysis)
{
    const std::atomic<bool> cancel{true};
    EXPECT_THROW(probeFile(sample("video_audio.mkv"), &cancel), ProbeCancelled);
}

TEST(MediaFormatting, Duration)
{
    EXPECT_EQ(formatDuration(0.0), "00:00:00.000");
    EXPECT_EQ(formatDuration(62.3456), "00:01:02.346");
    EXPECT_EQ(formatDuration(3 * 3600 + 5.5), "03:00:05.500");
    EXPECT_EQ(formatDuration(-1.0), "sconosciuta");
}

TEST(MediaFormatting, BitRate)
{
    EXPECT_EQ(formatBitRate(0), "sconosciuto");
    EXPECT_EQ(formatBitRate(128'000), "128 kb/s");
    EXPECT_EQ(formatBitRate(12'500'000), "12,5 Mb/s");
}

TEST(MediaFormatting, FrameRate)
{
    EXPECT_EQ(formatFrameRate(25.0), "25 fps");
    EXPECT_EQ(formatFrameRate(30000.0 / 1001.0), "29,97 fps");
    EXPECT_EQ(formatFrameRate(24000.0 / 1001.0), "23,976 fps");
    EXPECT_EQ(formatFrameRate(0.0), "sconosciuto");
}

TEST(MediaFormatting, StreamKindNames)
{
    EXPECT_STREQ(streamKindName(StreamKind::Video), "Video");
    EXPECT_STREQ(streamKindName(StreamKind::Subtitle), "Sottotitoli");
}

}  // namespace
