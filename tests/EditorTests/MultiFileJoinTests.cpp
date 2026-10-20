#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::audioTimeline;
using editor::tests::expectMonotonicPackets;
using editor::tests::expectVideoFrames;
using editor::tests::leftChannel;
using editor::tests::sampleAudio;
using editor::tests::SampleSpec;
using editor::tests::toUtf8;
using editor::tests::writeSampleFile;

namespace {

constexpr double kFrame = 1.0 / 25.0;
constexpr int kRate = 48000;
constexpr int kSamplesPerFrame = kRate / 25;

// Tre file da 2 s (50 fotogrammi, fotogrammi chiave a 0 e 1 s) con contenuti diversi: A parte dal fotogramma 0 dei
// file di prova, B dal 100, C dal 200. Così nel file unito si riconosce da quale file arriva ogni fotogramma.
constexpr int kFirst[] = {0, 100, 200};
const char* const kNames[] = {"a", "b", "c"};

std::vector<int> framesOf(int firstIndex, double start, double end)
{
    std::vector<int> frames;
    for (int i = static_cast<int>(std::lround(start / kFrame)); i < static_cast<int>(std::lround(end / kFrame)); ++i)
        frames.push_back(firstIndex + i);
    return frames;
}

std::vector<int> concat(std::initializer_list<std::vector<int>> parts)
{
    std::vector<int> all;
    for (const auto& p : parts)
        all.insert(all.end(), p.begin(), p.end());
    return all;
}

class MultiFileJoinTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoMultiFileTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!editor::tests::sampleEncodersAvailable())
            return;
        for (int i = 0; i < 3; ++i) {
            SampleSpec spec{.video = true, .audio = true, .frames = 50, .gopSize = 25};
            spec.firstIndex = kFirst[i];
            writeSampleFile(dir_ / (std::string(kNames[i]) + ".mkv"), spec);
            spec.aacAudio = true;
            writeSampleFile(dir_ / (std::string(kNames[i]) + ".mp4"), spec);
        }
        // File non compatibili con a.mkv: un dettaglio diverso ciascuno.
        writeSampleFile(dir_ / "risoluzione.mkv",
                        {.video = true, .audio = true, .frames = 50, .gopSize = 25, .width = 320, .height = 240});
        writeSampleFile(dir_ / "codec.mkv", {.video = true, .audio = true, .frames = 50, .gopSize = 25, .mpeg2 = true});
        SampleSpec rate{.video = true, .audio = true, .frames = 50, .gopSize = 25};
        rate.sampleRate = 44100;
        writeSampleFile(dir_ / "campionamento.mkv", rate);
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

    static std::string path(const std::string& name) { return toUtf8(dir_ / name); }

    // Il primo messaggio di incompatibilità fra a.mkv e `other`, e il controllo che l'unione venga rifiutata.
    static std::string refusal(const std::string& other)
    {
        const auto mismatches = checkCompatibility({readMediaSignature(path("a.mkv")), readMediaSignature(path(other))});
        EXPECT_FALSE(mismatches.empty());
        const std::string out = path("rifiutato.mkv");
        try {
            exportClips({{path("a.mkv"), {{0.0, 2.0}}}, {path(other), {{0.0, 2.0}}}}, out, AudioEdit{});
            ADD_FAILURE() << "l'unione doveva essere rifiutata";
        } catch (const MediaError& e) {
            EXPECT_NE(std::string(e.what()).find("non si possono unire"), std::string::npos) << e.what();
        }
        EXPECT_FALSE(fs::exists(dir_ / "rifiutato.mkv"));
        return mismatches.empty() ? std::string() : mismatches.front().message;
    }

    static inline fs::path dir_;
};

TEST_F(MultiFileJoinTest, ThreeWholeMatroskaFiles)
{
    std::vector<ClipRanges> clips;
    for (const char* name : kNames)
        clips.push_back({path(std::string(name) + ".mkv"), {{0.0, 2.0}}});
    const std::string out = path("unione.mkv");
    const ExportResult result = exportClips(clips, out, AudioEdit{});
    EXPECT_EQ(result.clipCount, 3);
    EXPECT_NEAR(result.durationSeconds, 6.0, 2 * kFrame);
    EXPECT_NEAR(probeFile(out).durationSeconds, 6.0, 2 * kFrame);

    expectVideoFrames(out, concat({framesOf(0, 0, 2), framesOf(100, 0, 2), framesOf(200, 0, 2)}));
    expectMonotonicPackets(out);

    // Audio PCM: a ogni giunzione si passa esattamente al primo campione del file successivo.
    const auto left = leftChannel(out);
    ASSERT_EQ(left.size(), static_cast<std::size_t>(6 * kRate));
    for (int k = 0; k < 3; ++k) {
        const std::size_t join = static_cast<std::size_t>(k) * 2 * kRate;
        EXPECT_EQ(left[join], sampleAudio(static_cast<long long>(kFirst[k]) * kSamplesPerFrame)) << "inizio file " << k + 1;
        EXPECT_EQ(left[join + 2 * kRate - 1], sampleAudio(static_cast<long long>(kFirst[k]) * kSamplesPerFrame + 2 * kRate - 1))
            << "fine file " << k + 1;
    }
}

TEST_F(MultiFileJoinTest, SegmentsInsideFiles)
{
    const std::vector<ClipRanges> clips = {
        {path("a.mkv"), {{0.0, 0.6}}},
        {path("b.mkv"), {{1.0, 1.4}}},
        {path("c.mkv"), {{0.0, 2.0}}},
    };
    const std::string out = path("tratti.mkv");
    const ExportResult result = exportClips(clips, out, AudioEdit{});
    EXPECT_EQ(result.segmentCount, 3);
    EXPECT_NEAR(probeFile(out).durationSeconds, 3.0, 2 * kFrame);
    expectVideoFrames(out, concat({framesOf(0, 0.0, 0.6), framesOf(100, 1.0, 1.4), framesOf(200, 0.0, 2.0)}));
    expectMonotonicPackets(out);
}

TEST_F(MultiFileJoinTest, ThreeMp4FilesWithAac)
{
    std::vector<ClipRanges> clips;
    for (const char* name : kNames)
        clips.push_back({path(std::string(name) + ".mp4"), {{0.0, 2.0}}});
    const std::string out = path("unione.mp4");
    exportClips(clips, out, AudioEdit{});

    const MediaInfo info = probeFile(out);
    EXPECT_NE(info.formatName.find("mp4"), std::string::npos);
    EXPECT_NEAR(info.durationSeconds, 6.0, 2 * kFrame);
    // Con AAC ogni file finisce con un blocco audio decodificato leggermente più lungo di quanto dichiarato
    // (riempimento dell'encoder): il file successivo parte pochi millesimi dopo, video e audio insieme.
    expectVideoFrames(out, concat({framesOf(0, 0, 2), framesOf(100, 0, 2), framesOf(200, 0, 2)}), 0.015);
    expectMonotonicPackets(out);

    // AAC: a ogni giunzione l'audio riparte entro un blocco (~21 ms) e i blocchi non si sovrappongono mai.
    const auto audio = audioTimeline(out);
    EXPECT_FALSE(audio.overlaps);
    for (double join : {0.0, 2.0, 4.0}) {
        const bool found = std::any_of(audio.starts.begin(), audio.starts.end(),
                                       [&](double t) { return t >= join - 0.001 && t <= join + 0.025; });
        EXPECT_TRUE(found) << "l'audio non riparte alla giunzione di " << join << " s";
    }
    EXPECT_GE(audio.totalSeconds, 6.0 - 3 * 2 * 0.0214);
    EXPECT_LE(audio.totalSeconds, 6.0 + 3 * 0.0214);
}

TEST_F(MultiFileJoinTest, SequenceOutOfOrderWithRepeatedFile)
{
    // Il montaggio: 4 pezzi da 3 file, in ordine sparso e con un file usato due volte, poi un pezzo spostato.
    Project project;
    std::vector<int> ids;
    for (const char* name : kNames) {
        const std::string file = path(std::string(name) + ".mkv");
        const MediaInfo info = probeFile(file);
        ids.push_back(project.add(file));
        project.setAnalysis(ids.back(), info.keyframeTimes, info.durationSeconds);
    }
    Sequence& s = project.sequence();
    const auto snap = project.snapper();
    s.insert(-1, ids[2], 1.3, 2.0, snap);              // c 1,0 .. 2,0 (agganciato)
    const int b = s.insert(-1, ids[1], 0.0, 0.4, snap);  // b 0 .. 0,4
    s.insert(-1, ids[0], 1.0, 1.6, snap);              // a 1,0 .. 1,6
    s.insert(-1, ids[2], 0.0, 0.8, snap);              // c di nuovo, 0 .. 0,8
    s.move(b, 3);                                      // b in fondo

    const std::string out = path("montaggio.mkv");
    exportClips(project.exportPlan(), out, AudioEdit{});
    EXPECT_NEAR(probeFile(out).durationSeconds, s.totalLength(), 2 * kFrame);
    expectVideoFrames(out, concat({framesOf(200, 1.0, 2.0), framesOf(0, 1.0, 1.6), framesOf(200, 0.0, 0.8),
                                   framesOf(100, 0.0, 0.4)}));
    expectMonotonicPackets(out);
}

TEST_F(MultiFileJoinTest, DifferentResolutionIsRefused)
{
    EXPECT_EQ(refusal("risoluzione.mkv"), "Clip 2 (risoluzione.mkv): risoluzione 320x240 invece di 64x48");
}

TEST_F(MultiFileJoinTest, DifferentCodecIsRefused)
{
    EXPECT_EQ(refusal("codec.mkv"), "Clip 2 (codec.mkv): codec video mpeg2video invece di mpeg4");
}

TEST_F(MultiFileJoinTest, DifferentSampleRateIsRefused)
{
    EXPECT_EQ(refusal("campionamento.mkv"),
              "Clip 2 (campionamento.mkv): frequenza di campionamento 44100 Hz invece di 48000 Hz");
}

TEST_F(MultiFileJoinTest, OutputMustDifferFromEveryInput)
{
    EXPECT_THROW(exportClips({{path("a.mkv"), {{0.0, 1.0}}}, {path("b.mkv"), {{0.0, 1.0}}}}, path("b.mkv"), AudioEdit{}),
                 MediaError);
    EXPECT_GT(fs::file_size(dir_ / "b.mkv"), 0u);
}

}  // namespace
