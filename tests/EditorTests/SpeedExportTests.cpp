#include "EditorCore/Encoder.h"
#include "EditorCore/LayerFrames.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/TextTracks.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::decodeVideoFrames;
using editor::tests::lumaAt;
using editor::tests::writeEncodedClip;

namespace {

// Velocità nell'esportazione (V5). File di prova H.264: 4 s a 30 fotogrammi al secondo, la luminanza cresce di 1 a ogni
// fotogramma (fotogramma i = 20 + i), così il fotogramma del file mostrato si legge dal valore.
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kBase = 20;

std::vector<int> lumas(const fs::path& path, int* errors = nullptr)
{
    std::vector<int> values;
    const int e = decodeVideoFrames(path, [&](const AVFrame& f) { values.push_back(lumaAt(f, 320, 180)); });
    if (errors != nullptr)
        *errors = e;
    return values;
}

class SpeedExport : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoVelocitaEsportazione";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeEncodedClip(dir_ / "contatore.mp4", {.fps = kFps, .frames = kFrames, .gop = kFps, .luma = kBase, .lumaStep = 1});
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove della velocità nell'esportazione si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    static inline fs::path dir_;
    static inline bool ok_ = false;
};

}  // namespace

TEST_F(SpeedExport, DoubleSpeedHalvesTheDurationAndSkipsFrames)
{
    ClipRanges clip{file("contatore.mp4"), {{0.0, 4.0}}};
    clip.speeds = {2.0};
    clip.reverses = {false};
    const std::vector<ClipRanges> plan{clip};

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, {});
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.speedSegments, 1);
    EXPECT_NEAR(preview.outputSeconds, 2.0, 0.01);

    const fs::path out = dir_ / "doppia.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.speedSegments, 1);
    int errors = 0;
    const std::vector<int> v = lumas(out, &errors);
    EXPECT_EQ(errors, 0);
    ASSERT_EQ(v.size(), static_cast<std::size_t>(60));
    EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, 2.0, 0.1);
    for (int k = 0; k < 60; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + 2 * k, 6) << "fotogramma " << k;
}

TEST_F(SpeedExport, HalfSpeedDoublesTheDurationAndRepeatsFrames)
{
    ClipRanges clip{file("contatore.mp4"), {{0.0, 2.0}}};
    clip.speeds = {0.5};
    clip.reverses = {false};
    const std::vector<ClipRanges> plan{clip};

    const fs::path out = dir_ / "meta.mp4";
    exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    const std::vector<int> v = lumas(out);
    ASSERT_EQ(v.size(), static_cast<std::size_t>(120));
    EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, 4.0, 0.1);
    for (int k = 0; k < 120; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + k / 2, 6) << "fotogramma " << k;
}

TEST_F(SpeedExport, OnlyTheSpedStretchChangesAndTheRestKeepsItsPlace)
{
    ClipRanges clip{file("contatore.mp4"), {{0.0, 1.0}, {1.0, 3.0}}};
    clip.speeds = {1.0, 2.0};
    clip.reverses = {false, false};
    const std::vector<ClipRanges> plan{clip};

    const fs::path out = dir_ / "misto.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.speedSegments, 1);
    const std::vector<int> v = lumas(out);
    ASSERT_EQ(v.size(), static_cast<std::size_t>(60));  // 1 s + 2 s / 2
    for (int k = 0; k < 30; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + k, 6) << "fotogramma " << k;
    for (int k = 30; k < 60; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + 30 + 2 * (k - 30), 6) << "fotogramma " << k;
}

TEST_F(SpeedExport, ReverseShowsTheFramesBackwards)
{
    ClipRanges clip{file("contatore.mp4"), {{0.0, 4.0}}};
    clip.speeds = {1.0};
    clip.reverses = {true};
    const std::vector<ClipRanges> plan{clip};
    EXPECT_TRUE(previewExport(plan, AudioEdit{}, nullptr, {}).problem.empty());

    const fs::path out = dir_ / "inverso.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.speedSegments, 1);
    int errors = 0;
    const std::vector<int> v = lumas(out, &errors);
    EXPECT_EQ(errors, 0);
    ASSERT_EQ(v.size(), static_cast<std::size_t>(kFrames));
    for (int k = 0; k < kFrames; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + (kFrames - 1 - k), 6) << "fotogramma " << k;
}

TEST_F(SpeedExport, ReverseInSeveralBlocksIsTheSameAsInOne)
{
    limitReverseBlockFrames(40);  // un blocco ogni ~1,3 s: il file di prova passa da più blocchi
    ClipRanges clip{file("contatore.mp4"), {{1.0, 3.0}}};
    clip.speeds = {1.0};
    clip.reverses = {true};
    const fs::path out = dir_ / "inverso_blocchi.mp4";
    exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    limitReverseBlockFrames(0);
    const std::vector<int> v = lumas(out);
    ASSERT_EQ(v.size(), static_cast<std::size_t>(60));
    for (int k = 0; k < 60; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + 89 - k, 6) << "fotogramma " << k;
}

TEST_F(SpeedExport, ReverseAtDoubleSpeedSkipsFramesBackwards)
{
    ClipRanges clip{file("contatore.mp4"), {{0.0, 4.0}}};
    clip.speeds = {2.0};
    clip.reverses = {true};
    const fs::path out = dir_ / "inverso_doppia.mp4";
    exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    const std::vector<int> v = lumas(out);
    ASSERT_EQ(v.size(), static_cast<std::size_t>(60));
    for (int k = 0; k < 60; ++k)
        EXPECT_NEAR(v[static_cast<std::size_t>(k)], kBase + 118 - 2 * k, 7) << "fotogramma " << k;
}

namespace {

// Valore (grigio) del centro di un fotogramma BGRA del livello: cresce con il numero del fotogramma del file di prova.
int grayOf(const VideoFrame& f)
{
    const std::size_t at = static_cast<std::size_t>(f.height / 2) * static_cast<std::size_t>(f.stride) + static_cast<std::size_t>(f.width / 2) * 4;
    return f.pixels[at + 1];
}

}  // namespace

TEST_F(SpeedExport, LayerPiecesFollowSpeedAndReverseInTheExport)
{
    TextTracks tracks;
    const int layer = tracks.addLayer();
    const auto add = [&](double speed, bool reverse, double position) {
        LayerPiece piece;
        piece.clipId = 1;
        piece.start = 0.0;
        piece.end = 4.0;
        piece.speed = speed;
        piece.reverse = reverse;
        piece.position = position;
        return tracks.addPiece(layer, piece);
    };
    add(2.0, false, 0.0);   // 0 - 2 s: il file a doppia velocità
    add(1.0, true, 10.0);   // 10 - 14 s: il file all'indietro
    // Riferimento: lo stesso file a velocità normale, pezzo che inizia a 0 s (il tempo di montaggio è il tempo del file).
    TextTracks plainTracks;
    const int plainLayer = plainTracks.addLayer();
    LayerPiece plain;
    plain.clipId = 1;
    plain.start = 0.0;
    plain.end = 4.0;
    plainTracks.addPiece(plainLayer, plain);
    LayerFrameSource source({{1, file("contatore.mp4")}}, 640, 360, false);
    LayerFrameSource reference({{1, file("contatore.mp4")}}, 640, 360, false);
    const auto gray = [&](double t) {
        const auto visible = source.at(tracks, t);
        return visible.empty() ? -1 : grayOf(*visible.front().frame);
    };
    const auto expected = [&](double fileTime) {
        const auto visible = reference.at(plainTracks, fileTime);
        return visible.empty() ? -2 : grayOf(*visible.front().frame);
    };
    // A 2x, a 0,5 s di montaggio si vede il file a 1 s; a 1,5 s il file a 3 s.
    EXPECT_NEAR(gray(0.5), expected(1.0), 8);
    EXPECT_NEAR(gray(1.5), expected(3.0), 8);
    // Inverso: a 1 s dall'inizio del pezzo si vede il file a 3 s, a 0,5 s quello a 3,5 s, a 3 s quello a 1 s.
    EXPECT_NEAR(gray(11.0), expected(3.0), 8);
    EXPECT_NEAR(gray(10.5), expected(3.5), 8);
    EXPECT_NEAR(gray(13.0), expected(1.0), 8);
    EXPECT_NEAR(gray(12.0), expected(2.0), 8);  // di nuovo dentro il blocco già letto
    EXPECT_GT(gray(11.0), gray(13.0) + 20);       // e i valori sono davvero in ordine inverso
}
