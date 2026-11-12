#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
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

TEST_F(SpeedExport, ReverseIsNotExportableYet)
{
    ClipRanges clip{file("contatore.mp4"), {{0.0, 1.0}}};
    clip.speeds = {1.0};
    clip.reverses = {true};
    EXPECT_FALSE(previewExport({clip}, AudioEdit{}, nullptr, {}).problem.empty());
}
