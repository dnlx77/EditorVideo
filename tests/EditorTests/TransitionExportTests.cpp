#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::decodeVideoFrames;
using editor::tests::lumaAt;
using editor::tests::writeEncodedClip;

namespace {

// Transizioni nell'esportazione (step X8). Due file H.264 di 4 s a 30 fotogrammi al secondo, un fotogramma chiave al
// secondo, luminanza uniforme (60 il primo, 200 il secondo), audio AAC.
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kWidth = 640;
constexpr int kHeight = 360;

bool writeClip(const fs::path& path, std::uint8_t luma)
{
    return writeEncodedClip(path, {.width = kWidth, .height = kHeight, .fps = kFps, .frames = kFrames, .gop = kFps, .luma = luma});
}


// La luminanza al centro di ogni fotogramma del file.
std::vector<int> centerLuma(const fs::path& path, int* errors)
{
    std::vector<int> result;
    *errors += decodeVideoFrames(path, [&](const AVFrame& f) { result.push_back(lumaAt(f, kWidth / 2, kHeight / 2)); });
    return result;
}

class TransitionExport : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoTransizioniEsportazione";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeClip(dir_ / "scuro.mp4", 60) && writeClip(dir_ / "chiaro.mp4", 200);
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove delle transizioni nell'esportazione si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    // Scuro 0-2 s, poi chiaro 1-3 s, con la transizione `type` di `duration` secondi sulla giunzione (a 2 s).
    static Project montage(const char* type, double duration, int* transitionId = nullptr)
    {
        Project project;
        const int a = project.add(file("scuro.mp4"));
        const int b = project.add(file("chiaro.mp4"));
        project.setAnalysis(a, {0.0, 1.0, 2.0, 3.0}, 4.0);
        project.setAnalysis(b, {0.0, 1.0, 2.0, 3.0}, 4.0);
        const int first = project.sequence().insert(-1, a, 0.0, 2.0);
        project.sequence().insert(-1, b, 1.0, 3.0);
        const int id = project.addTransition(first, type, duration);
        if (transitionId != nullptr)
            *transitionId = id;
        return project;
    }

    static inline fs::path dir_;
    static inline bool ok_ = false;
};

}  // namespace

TEST_F(TransitionExport, OnlyTheStretchesInTheTransitionAreReencodedAndBlended)
{
    Project project = montage("dissolvenza", 1.0);
    ASSERT_EQ(project.activeTransitions().size(), 1u);
    ReencodeOptions options;
    options.transitions = exportTransitionsOf(project);
    ASSERT_EQ(options.transitions.size(), 1u);
    EXPECT_NEAR(options.transitions[0].before, 0.5, 1e-9);
    EXPECT_NEAR(options.transitions[0].after, 0.5, 1e-9);
    const auto plan = project.exportPlan(GapExport::Black);

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, options);
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.transitionSegments, 2);
    EXPECT_NEAR(preview.transitionSeconds, 1.0, 0.01);
    EXPECT_NEAR(preview.outputSeconds, 4.0, 0.01);

    const fs::path out = dir_ / "dissolvenza.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.transitionSegments, 2);
    int errors = 0;
    const std::vector<int> luma = centerLuma(out, &errors);
    EXPECT_EQ(errors, 0);
    ASSERT_EQ(luma.size(), static_cast<std::size_t>(kFrames));
    EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, 4.0, 0.05);
    for (int i = 0; i < kFrames; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const double t = (i + 0.5) / kFps;  // nel montaggio
        double expected;
        if (t < 1.5)
            expected = 60;
        else if (t > 2.5)
            expected = 200;
        else
            expected = 60 + 140 * (t - 1.5);  // dissolvenza lineare da 1,5 a 2,5 s
        EXPECT_NEAR(luma[static_cast<std::size_t>(i)], expected, 9);
    }
    // A metà esatta i due pezzi pesano uguale.
    EXPECT_NEAR(luma[60], 130, 9);
}

TEST_F(TransitionExport, WithoutTheTransitionTheCutIsSharp)
{
    Project project = montage("dissolvenza", 1.0);
    const auto plan = project.exportPlan(GapExport::Black);
    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, ReencodeOptions{});
    EXPECT_EQ(preview.transitionSegments, 0);
    const fs::path out = dir_ / "taglio.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {});
    EXPECT_EQ(result.transitionSegments, 0);
    int errors = 0;
    const std::vector<int> luma = centerLuma(out, &errors);
    ASSERT_EQ(luma.size(), static_cast<std::size_t>(kFrames));
    EXPECT_NEAR(luma[55], 60, 9);
    EXPECT_NEAR(luma[65], 200, 9);
}

TEST_F(TransitionExport, DipToBlackGoesThroughBlackAndSlideMovesThePieces)
{
    for (const char* type : {"dissolvenza-nero", "scorrimento"}) {
        SCOPED_TRACE(type);
        Project project = montage(type, 1.0);
        ASSERT_EQ(project.activeTransitions().size(), 1u);
        ReencodeOptions options;
        options.transitions = exportTransitionsOf(project);
        const fs::path out = dir_ / (std::string(type) + ".mp4");
        const ExportResult result =
            exportClips(project.exportPlan(GapExport::Black), utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
        EXPECT_EQ(result.transitionSegments, 2);
        int errors = 0;
        const std::vector<int> luma = centerLuma(out, &errors);
        EXPECT_EQ(errors, 0);
        ASSERT_EQ(luma.size(), static_cast<std::size_t>(kFrames));
        EXPECT_NEAR(luma[40], 60, 9);
        EXPECT_NEAR(luma[80], 200, 9);
        if (std::string(type) == "dissolvenza-nero")
            EXPECT_LT(luma[60], 40);  // a metà è nero (TV: 16)
    }
}

TEST_F(TransitionExport, PiecesAtDoubleSpeedCanHaveATransition)
{
    // Scuro [0, 3) a 2x (1,5 s nel montaggio) e chiaro [1, 3) a 2x (1 s): i margini sono 0,5 s dopo il primo (1 s di file) e
    // 0,5 s prima del secondo (1 s di file), quindi la transizione di 1 s sta a cavallo della giunzione a 1,5 s.
    Project project;
    const int a = project.add(file("scuro.mp4"));
    const int b = project.add(file("chiaro.mp4"));
    project.setAnalysis(a, {0.0, 1.0, 2.0, 3.0}, 4.0);
    project.setAnalysis(b, {0.0, 1.0, 2.0, 3.0}, 4.0);
    const int first = project.sequence().insert(-1, a, 0.0, 3.0, 2.0);
    project.sequence().insert(-1, b, 1.0, 3.0, 2.0);
    ASSERT_NE(project.addTransition(first, "dissolvenza", 1.0), 0);
    ReencodeOptions options;
    options.transitions = exportTransitionsOf(project);
    ASSERT_EQ(options.transitions.size(), 1u);
    const auto plan = project.exportPlan(GapExport::Black);

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, options);
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_NEAR(preview.outputSeconds, 2.5, 0.05);

    const fs::path out = dir_ / "veloce.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.transitionSegments, 2);
    int errors = 0;
    const std::vector<int> luma = centerLuma(out, &errors);
    EXPECT_EQ(errors, 0);
    ASSERT_EQ(luma.size(), static_cast<std::size_t>(75));
    for (int i = 0; i < 75; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const double t = (i + 0.5) / kFps;
        const double expected = t < 1.0 ? 60 : t > 2.0 ? 200 : 60 + 140 * (t - 1.0);
        EXPECT_NEAR(luma[static_cast<std::size_t>(i)], expected, 12);
    }
}

TEST_F(TransitionExport, ReversedPiecesCanHaveATransitionToo)
{
    // Come sopra ma all'inverso: i margini stanno dall'altra parte nel file. Scuro [1, 4) a 2x (1,5 s nel montaggio, 1 s di file
    // prima dell'inizio = 0,5 s dopo nel montaggio) e chiaro [0, 2) a 2x (1 s, con 2 s di file dopo la fine).
    Project project;
    const int a = project.add(file("scuro.mp4"));
    const int b = project.add(file("chiaro.mp4"));
    project.setAnalysis(a, {0.0, 1.0, 2.0, 3.0}, 4.0);
    project.setAnalysis(b, {0.0, 1.0, 2.0, 3.0}, 4.0);
    const int first = project.sequence().insert(-1, a, 1.0, 4.0, 2.0, true);
    project.sequence().insert(-1, b, 0.0, 2.0, 2.0, true);
    ASSERT_NE(project.addTransition(first, "dissolvenza", 1.0), 0);
    ReencodeOptions options;
    options.transitions = exportTransitionsOf(project);
    ASSERT_EQ(options.transitions.size(), 1u);
    const auto plan = project.exportPlan(GapExport::Black);

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, options);
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_NEAR(preview.outputSeconds, 2.5, 0.05);

    const fs::path out = dir_ / "inverso.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.transitionSegments, 2);
    int errors = 0;
    const std::vector<int> luma = centerLuma(out, &errors);
    EXPECT_EQ(errors, 0);
    ASSERT_EQ(luma.size(), static_cast<std::size_t>(75));
    for (int i = 0; i < 75; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const double t = (i + 0.5) / kFps;
        const double expected = t < 1.0 ? 60 : t > 2.0 ? 200 : 60 + 140 * (t - 1.0);
        EXPECT_NEAR(luma[static_cast<std::size_t>(i)], expected, 12);
    }
}
