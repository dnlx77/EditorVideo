#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Transform.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::decodeVideoFrames;
using editor::tests::lumaAt;
using editor::tests::writeEncodedClip;

namespace {

// Trasformazioni nell'esportazione (step TR4). File di prova H.264: 4 s a 30 fotogrammi al secondo, un fotogramma chiave al
// secondo, luminanza uniforme 100, audio AAC. 
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kWidth = 640;
constexpr int kHeight = 360;

bool writeClip(const fs::path& path, int luma = 100)
{
    return writeEncodedClip(path, {.width = kWidth, .height = kHeight, .fps = kFps, .frames = kFrames, .gop = kFps, .luma = luma});
}

struct Sampled
{
    std::vector<std::array<int, 3>> pixels;  // per fotogramma: centro, angolo in alto a sinistra, centro del quarto destro
    int errors = 0;
};

Sampled sample(const fs::path& path)
{
    Sampled d;
    d.errors = decodeVideoFrames(path, [&](const AVFrame& f) {
        d.pixels.push_back({lumaAt(f, kWidth / 2, kHeight / 2), lumaAt(f, 20, 20), lumaAt(f, kWidth * 3 / 4 + 40, kHeight / 2)});
    });
    return d;
}

class TransformExport : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoTrasformazioniEsportazione";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeClip(dir_ / "grigio.mp4") && writeClip(dir_ / "chiaro.mp4", 200);
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove delle trasformazioni nell'esportazione si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    static inline fs::path dir_;
    static inline bool ok_ = false;
};

}  // namespace

TEST_F(TransformExport, OnlyTheTransformedStretchIsReencodedAndDrawnSmaller)
{
    ClipRanges clip{file("grigio.mp4"), {{0.0, 2.0}, {2.0, 4.0}}};
    clip.transforms = {Transform{}, Transform{}};
    clip.transforms[1].scaleX = clip.transforms[1].scaleY = 0.5;
    const std::vector<ClipRanges> plan{clip};

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, {});
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.transformSegments, 1);
    EXPECT_NEAR(preview.transformSeconds, 2.0, 0.01);

    const fs::path out = dir_ / "scala.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.transformSegments, 1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, 4.0, 0.05);
    for (int i = 0; i < kFrames; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const auto& p = d.pixels[static_cast<std::size_t>(i)];
        if (i < 58) {  // non trasformato: grigio dappertutto
            EXPECT_NEAR(p[0], 100, 6);
            EXPECT_NEAR(p[1], 100, 6);
            EXPECT_NEAR(p[2], 100, 6);
        } else if (i > 62) {  // metà scala: il centro è grigio, gli angoli e il quarto destro sono neri
            EXPECT_NEAR(p[0], 100, 6);
            EXPECT_LT(p[1], 30);
            EXPECT_LT(p[2], 30);
        }
    }
}

TEST_F(TransformExport, PositionMovesThePicture)
{
    ClipRanges clip{file("grigio.mp4"), {{0.0, 4.0}}};
    clip.transforms = {Transform{}};
    clip.transforms[0].posX = kWidth / 2.0;  // tutta l'immagine spostata a destra della metà: la parte a sinistra resta nera
    const std::vector<ClipRanges> plan{clip};

    const fs::path out = dir_ / "posizione.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.transformSegments, 1);
    const Sampled d = sample(out);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    for (std::size_t i = 4; i < d.pixels.size(); ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        EXPECT_LT(d.pixels[i][1], 30);           // a sinistra niente
        EXPECT_NEAR(d.pixels[i][2], 100, 6);     // a destra c'è il video
    }
}

TEST_F(TransformExport, NoTransformMeansEverythingIsCopied)
{
    ClipRanges clip{file("grigio.mp4"), {{0.0, 4.0}}};
    clip.transforms = {Transform{}};
    const ExportPreview preview = previewExport({clip}, AudioEdit{}, nullptr, {});
    EXPECT_EQ(preview.transformSegments, 0);
    EXPECT_EQ(preview.copiedSegments, 1);
}

TEST_F(TransformExport, LayerPiecesAreComposedOverTheMainVideo)
{
    // V1: il grigio (luminanza 100) per 4 s. Sul livello V2, dal secondo 1 al 3, un pezzo del file chiaro (200) a metà scala.
    ClipRanges clip{file("grigio.mp4"), {{0.0, 4.0}}};
    clip.transforms = {Transform{}};
    ReencodeOptions options;
    const int layer = options.texts.addLayer();
    LayerPiece piece;
    piece.clipId = 7;
    piece.start = 0.0;
    piece.end = 2.0;
    piece.position = 1.0;
    piece.transform.scaleX = piece.transform.scaleY = 0.5;
    ASSERT_NE(options.texts.addPiece(layer, piece), 0);
    options.layerFiles[7] = file("chiaro.mp4");

    const ExportPreview preview = previewExport({clip}, AudioEdit{}, nullptr, options);
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.textSegments, 1);
    EXPECT_NEAR(preview.textSeconds, 2.0, 0.01);

    const fs::path out = dir_ / "livelli.mp4";
    const ExportResult result = exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.textSegments, 1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const auto& p = d.pixels[static_cast<std::size_t>(i)];
        EXPECT_NEAR(p[1], 100, 8);      // l'angolo è sempre V1
        EXPECT_NEAR(p[2], 100, 8);      // e così il quarto destro (il pezzo è a metà scala, al centro)
        if (i < 28 || i > 92)
            EXPECT_NEAR(p[0], 100, 8);  // fuori dal pezzo: solo V1
        else if (i > 32 && i < 88)
            EXPECT_NEAR(p[0], 200, 8);  // dentro: il pezzo chiaro al centro
    }
}

TEST_F(TransformExport, LayerPieceOverAGapIsDrawnOnBlack)
{
    // V1: due tratti con uno spazio vuoto di 2 s in mezzo; il pezzo del livello sta nello spazio.
    ClipRanges clip{file("grigio.mp4"), {{0.0, 1.0}, {1.0, 2.0}}};
    clip.gapsBefore = {0.0, 2.0};
    clip.transforms = {Transform{}, Transform{}};
    ReencodeOptions options;
    const int layer = options.texts.addLayer();
    LayerPiece piece;
    piece.clipId = 7;
    piece.start = 0.0;
    piece.end = 2.0;
    piece.position = 1.0;
    ASSERT_NE(options.texts.addPiece(layer, piece), 0);
    options.layerFiles[7] = file("chiaro.mp4");
    const fs::path out = dir_ / "livelloNelloSpazio.mp4";
    exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    const Sampled d = sample(out);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(4 * kFps));
    // Nello spazio (1..3 s) c'è il pezzo chiaro a tutto schermo, non il nero.
    EXPECT_NEAR(d.pixels[2 * kFps][0], 200, 8);
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(kFps / 2)][0], 100, 8);
}

TEST_F(TransformExport, LayerPieceBeyondTheEndOfV1IsExportedOnBlack)
{
    // V1 dura 2 s; il pezzo del livello va da 1 s a 4 s: oltre V1 c'è una coda nera di 2 s con il pezzo sopra.
    ClipRanges clip{file("grigio.mp4"), {{0.0, 2.0}}};
    clip.gapsBefore = {0.0};
    clip.transforms = {Transform{}};
    ReencodeOptions options;
    const int layer = options.texts.addLayer();
    LayerPiece piece;
    piece.clipId = 7;
    piece.start = 0.0;
    piece.end = 3.0;
    piece.position = 1.0;
    ASSERT_NE(options.texts.addPiece(layer, piece), 0);
    options.layerFiles[7] = file("chiaro.mp4");
    options.tailSeconds = 2.0;
    const fs::path out = dir_ / "livelloOltreV1.mp4";
    const ExportPreview preview = previewExport({clip}, AudioEdit{}, nullptr, options);
    EXPECT_NEAR(preview.outputSeconds, 4.0, 0.1);
    exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    const Sampled d = sample(out);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(4 * kFps));
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(kFps / 2)][0], 100, 8);  // V1 da solo
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(3 * kFps)][0], 200, 8);  // la coda: il pezzo del livello
}

TEST_F(TransformExport, AnimatedOpacityIsEvaluatedFrameByFrame)
{
    // Un tratto di 4 s con l'opacità che scende da 1 a 0 (tempi nel file): il centro passa da luminanza 100 al nero (16 nel video
    // a gamma limitata), in modo lineare.
    ClipRanges clip{file("grigio.mp4"), {{0.0, 4.0}}};
    clip.transforms = {Transform{}};
    TransformAnimation animation;
    animation.track(AnimParam::Opacity) = {{.time = 0.0, .value = 1.0}, {.time = 4.0, .value = 0.0}};
    clip.animations = {animation};
    const std::vector<ClipRanges> plan{clip};

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, {});
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.transformSegments, 1);  // con i fotogrammi chiave il tratto si ricodifica
    EXPECT_NEAR(preview.transformSeconds, 4.0, 0.01);

    const fs::path out = dir_ / "opacitaAnimata.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.transformSegments, 1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    for (int i = 4; i < kFrames - 4; i += 7) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        EXPECT_NEAR(d.pixels[static_cast<std::size_t>(i)][0], 16.0 + 84.0 * (1.0 - static_cast<double>(i) / kFrames), 7.0);  // (il nero è 16)
    }
}

TEST_F(TransformExport, AnimationKeysFollowTheFileTimeOfTheStretch)
{
    // Solo il secondo tratto (file 2..4 s) è animato: la dissolvenza va da 1 (a 2 s del file) a 0 (a 4 s). Il primo resta com'è.
    ClipRanges clip{file("grigio.mp4"), {{0.0, 2.0}, {2.0, 4.0}}};
    clip.transforms = {Transform{}, Transform{}};
    TransformAnimation animation;
    animation.track(AnimParam::Opacity) = {{.time = 2.0, .value = 1.0}, {.time = 4.0, .value = 0.0}};
    clip.animations = {TransformAnimation{}, animation};
    const fs::path out = dir_ / "opacitaSecondoTratto.mp4";
    const ExportResult result = exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, {});
    EXPECT_EQ(result.transformSegments, 1);
    const Sampled d = sample(out);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(kFps)][0], 100, 6);         // primo tratto: pieno
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(2 * kFps + 5)][0], 16.0 + 84.0 * (1.0 - 5.0 / 60.0), 7.0);  // inizio della dissolvenza
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(3 * kFps)][0], 58, 7);      // a metà (file 3 s)
}

TEST_F(TransformExport, AnimatedLayerPieceIsEvaluatedFrameByFrame)
{
    // V1 grigio (100) per 4 s; sul livello, da 1 s a 3 s, il pezzo chiaro (200) la cui opacità scende da 1 a 0: 200 → 100.
    ClipRanges clip{file("grigio.mp4"), {{0.0, 4.0}}};
    clip.transforms = {Transform{}};
    ReencodeOptions options;
    const int layer = options.texts.addLayer();
    LayerPiece piece;
    piece.clipId = 7;
    piece.start = 0.0;
    piece.end = 2.0;
    piece.position = 1.0;
    piece.animation.track(AnimParam::Opacity) = {{.time = 0.0, .value = 1.0}, {.time = 2.0, .value = 0.0}};
    ASSERT_NE(options.texts.addPiece(layer, piece), 0);
    options.layerFiles[7] = file("chiaro.mp4");
    const fs::path out = dir_ / "livelloAnimato.mp4";
    exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    const Sampled d = sample(out);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(kFps + 3)][0], 200, 10);      // all'inizio del pezzo: opaco
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(2 * kFps)][0], 150, 10);      // a metà: metà e metà
    EXPECT_NEAR(d.pixels[static_cast<std::size_t>(3 * kFps - 3)][0], 100, 10);  // alla fine: trasparente
}
