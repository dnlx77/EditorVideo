#include "EditorCore/AudioDecoder.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/VideoDecoder.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::sampleAudio;
using editor::tests::sampleLuma;
using editor::tests::SampleSpec;
using editor::tests::toUtf8;
using editor::tests::writeSampleFile;

namespace {

constexpr double kFrame = 1.0 / 25.0;

// File di prova da 4 s (100 fotogrammi), fotogrammi chiave ogni secondo (0, 1, 2, 3).
// Il segmento usato nei test: IN chiesto a 1,3 s (-> fotogramma chiave 1,0 s), OUT a 2,6 s => 1,6 s, fotogrammi 25..64.
constexpr double kWantedIn = 1.3;
constexpr double kSnappedIn = 1.0;
constexpr double kOut = 2.6;
constexpr int kFirstFrame = 25;
constexpr int kFrameCount = 40;

class ExportTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoExportTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!editor::tests::sampleEncodersAvailable())
            return;
        const SampleSpec base{.video = true, .audio = true, .frames = 100, .gopSize = 25};
        writeSampleFile(dir_ / "sorgente.mkv", base);
        SampleSpec mp4 = base;
        mp4.aacAudio = true;
        writeSampleFile(dir_ / "sorgente.mp4", mp4);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void SetUp() override
    {
        if (!editor::tests::sampleEncodersAvailable())
            GTEST_SKIP() << "FFmpeg senza gli encoder necessari: impossibile generare i file di prova";
    }

    static std::string path(const fs::path& name) { return toUtf8(dir_ / name); }

    // Segmento come lo costruisce l'interfaccia: fotogrammi chiave dall'analisi del file, IN agganciato.
    static CutSegment segmentFor(const std::string& source)
    {
        const MediaInfo info = probeFile(source);
        CutSegment segment;
        segment.reset(info.keyframeTimes, info.durationSeconds);
        EXPECT_DOUBLE_EQ(segment.setIn(kWantedIn), kSnappedIn);
        EXPECT_TRUE(segment.setOut(kOut));
        return segment;
    }

    static double lumaAt(const VideoFrame& frame, int x, int y)
    {
        const std::uint8_t* p = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride + x * 4;
        return 16.0 + (p[0] + p[1] + p[2]) / 3.0 * 219.0 / 255.0;
    }

    // Il file esportato: stessi codec e contenitore, durata giusta, parte da un fotogramma chiave a 0,
    // il primo fotogramma è quello in IN, ci sono esattamente i fotogrammi del segmento.
    static void checkExportedVideo(const std::string& source, const std::string& exported, const char* format)
    {
        const MediaInfo in = probeFile(source);
        const MediaInfo out = probeFile(exported);
        EXPECT_EQ(out.formatName, in.formatName);
        EXPECT_NE(out.formatName.find(format), std::string::npos);
        ASSERT_EQ(out.streams.size(), in.streams.size());
        for (std::size_t i = 0; i < in.streams.size(); ++i) {
            EXPECT_EQ(out.streams[i].codec, in.streams[i].codec);
            EXPECT_EQ(out.streams[i].kind, in.streams[i].kind);
        }
        EXPECT_NEAR(out.durationSeconds, kOut - kSnappedIn, 2 * kFrame);

        ASSERT_FALSE(out.keyframeTimes.empty());
        EXPECT_NEAR(out.keyframeTimes.front(), 0.0, 0.001);  // il primo pacchetto video è un fotogramma chiave

        VideoDecoder video(exported, {.allowHardware = false});
        auto frame = video.seek(0.0);
        int count = 0;
        while (frame) {
            const int x = frame->width / 2;
            const int y = frame->height / 2;
            EXPECT_NEAR(frame->time, count * kFrame, 0.001);
            EXPECT_NEAR(lumaAt(*frame, x, y), sampleLuma(x, y, kFirstFrame + count), 2.5)
                << "il fotogramma " << count << " dovrebbe essere il " << kFirstFrame + count << " dell'originale";
            ++count;
            frame = video.nextFrame();
        }
        EXPECT_EQ(count, kFrameCount);
    }

    static inline fs::path dir_;
};

TEST_F(ExportTest, MatroskaSegmentKeepsCodecsAndSync)
{
    const std::string source = path("sorgente.mkv");
    const std::string exported = path("taglio.mkv");
    const ExportResult result = exportSegment(source, exported, segmentFor(source));
    EXPECT_EQ(result.streamCount, 2);
    EXPECT_NEAR(result.durationSeconds, kOut - kSnappedIn, 2 * kFrame);

    checkExportedVideo(source, exported, "matroska");

    // Audio PCM: si può controllare campione per campione che parta esattamente da IN, in sincrono col video.
    AudioDecoder audio(exported);
    auto chunk = audio.next();
    ASSERT_TRUE(chunk.has_value());
    EXPECT_NEAR(chunk->time, 0.0, kFrame);
    const auto firstSample = std::llround((kSnappedIn + chunk->time) * 48000);
    EXPECT_EQ(chunk->samples[0], sampleAudio(firstSample));
    long long samples = static_cast<long long>(chunk->samples.size() / 2);
    while (auto more = audio.next())
        samples += static_cast<long long>(more->samples.size() / 2);
    EXPECT_NEAR(samples / 48000.0, kOut - kSnappedIn, kFrame);
}

TEST_F(ExportTest, Mp4SegmentWithAacKeepsCodecsAndSync)
{
    const std::string source = path("sorgente.mp4");
    const std::string exported = path("taglio.mp4");
    exportSegment(source, exported, segmentFor(source));

    checkExportedVideo(source, exported, "mp4");

    // AAC: audio e video devono partire insieme, entro un fotogramma.
    AudioDecoder audio(exported);
    auto chunk = audio.next();
    ASSERT_TRUE(chunk.has_value());
    EXPECT_NEAR(chunk->time, 0.0, kFrame);
}

TEST_F(ExportTest, PathWithAccentedCharacters)
{
    const std::string source = path("sorgente.mkv");
    const std::string exported = path(u8"taglio perché è così.mkv");
    exportSegment(source, exported, segmentFor(source));
    EXPECT_TRUE(fs::exists(dir_ / u8"taglio perché è così.mkv"));
    EXPECT_NEAR(probeFile(exported).durationSeconds, kOut - kSnappedIn, 2 * kFrame);
}

TEST_F(ExportTest, OutNotAfterInIsRejected)
{
    const std::string source = path("sorgente.mkv");
    const std::string exported = path("errore.mkv");
    EXPECT_THROW(exportSegment(source, exported, 2.0, 2.0), MediaError);
    EXPECT_THROW(exportSegment(source, exported, 2.0, 1.0), MediaError);
    EXPECT_FALSE(fs::exists(dir_ / "errore.mkv"));
}

TEST_F(ExportTest, SegmentWithoutInAndOutIsRejected)
{
    const std::string source = path("sorgente.mkv");
    CutSegment segment;
    segment.reset({0.0, 1.0, 2.0, 3.0}, 4.0);
    segment.setIn(1.0);  // manca OUT
    try {
        exportSegment(source, path("errore.mkv"), segment);
        FAIL() << "doveva rifiutare";
    } catch (const MediaError& e) {
        EXPECT_NE(std::string(e.what()).find("IN e OUT"), std::string::npos) << e.what();
    }
}

TEST_F(ExportTest, CannotOverwriteSource)
{
    const std::string source = path("sorgente.mkv");
    EXPECT_THROW(exportSegment(source, source, 1.0, 2.0), MediaError);
    EXPECT_GT(fs::file_size(dir_ / "sorgente.mkv"), 0u);  // l'originale è intatto
}

TEST_F(ExportTest, ContainerThatCannotHoldTheCodecIsRejected)
{
    // PCM in un WebM non è ammesso: errore chiaro e nessun file a metà.
    const std::string exported = path("errore.webm");
    try {
        exportSegment(path("sorgente.mkv"), exported, 1.0, 2.0);
        FAIL() << "doveva rifiutare";
    } catch (const MediaError& e) {
        EXPECT_NE(std::string(e.what()).find("non può contenere"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(dir_ / "errore.webm"));
}

TEST_F(ExportTest, ProgressIsReportedAndCanCancel)
{
    const std::string source = path("sorgente.mkv");
    double last = -1.0;
    bool increasing = true;
    exportSegment(source, path("progresso.mkv"), 1.0, 3.0, [&](double fraction) {
        increasing = increasing && fraction >= last - 1e-9;
        last = fraction;
        return true;
    });
    EXPECT_TRUE(increasing);
    EXPECT_DOUBLE_EQ(last, 1.0);

    // Annullando a metà non resta niente su disco.
    EXPECT_THROW(exportSegment(source, path("annullato.mkv"), 1.0, 3.0, [](double fraction) { return fraction < 0.5; }),
                 ExportCancelled);
    EXPECT_FALSE(fs::exists(dir_ / "annullato.mkv"));
}

}  // namespace
