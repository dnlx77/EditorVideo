#include "EditorCore/Animation.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/TextFrame.h"
#include "EditorCore/TextImage.h"
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

// Scritte nell'esportazione (step T6). File di prova H.264: 4 s a 30 fotogrammi al secondo, un fotogramma chiave al
// secondo, luminanza uniforme 100, audio AAC. Le scritte sono un rettangolo bianco pieno disegnato da un TextRenderer finto.
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kBoxX = 200;
constexpr int kBoxY = 100;
constexpr int kBoxW = 120;
constexpr int kBoxH = 60;

class WhiteBox : public TextRenderer
{
public:
    TextImage render(const TextClip&, int, int) const override
    {
        TextImage image;
        image.x = kBoxX;
        image.y = kBoxY;
        image.width = kBoxW;
        image.height = kBoxH;
        image.rgba.assign(static_cast<std::size_t>(kBoxW) * kBoxH * 4, 255);
        return image;
    }
};

// Come WhiteBox, ma conta quante volte la scritta viene disegnata.
class CountingBox : public WhiteBox
{
public:
    TextImage render(const TextClip& clip, int w, int h) const override
    {
        ++renders;
        return WhiteBox::render(clip, w, h);
    }
    mutable int renders = 0;
};

bool writeClip(const fs::path& path)
{
    return writeEncodedClip(path, {.width = kWidth, .height = kHeight, .fps = kFps, .frames = kFrames, .gop = kFps, .luma = 100});
}

struct Sampled
{
    std::vector<int> inside;   // luminanza dentro il rettangolo della scritta, per fotogramma
    std::vector<int> outside;  // luminanza fuori
    int errors = 0;
};

Sampled sample(const fs::path& path)
{
    Sampled d;
    d.errors = decodeVideoFrames(path, [&](const AVFrame& f) {
        d.inside.push_back(lumaAt(f, kBoxX + kBoxW / 2, kBoxY + kBoxH / 2));
        d.outside.push_back(lumaAt(f, 20, 20));
    });
    return d;
}

TextTracks oneText(double position, double length)
{
    TextTracks tracks;
    const int layer = tracks.addLayer();
    TextClip clip;
    clip.position = position;
    clip.length = length;
    clip.text = "A";
    tracks.add(layer, clip);
    return tracks;
}

class TextExport : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoScritteEsportazione";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeClip(dir_ / "grigio.mp4");
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove delle scritte nell'esportazione si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    static inline fs::path dir_;
    static inline bool ok_ = false;
};

}  // namespace

TEST_F(TextExport, OnlyTheFramesUnderTheTextAreReencodedWithTheTextOnTop)
{
    const WhiteBox renderer;
    ReencodeOptions options;
    options.texts = oneText(1.2, 1.4);  // fotogrammi 36 .. 77
    options.textRenderer = &renderer;
    const std::vector<ClipRanges> plan{{file("grigio.mp4"), {{0.0, 4.0}}}};

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, options);
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.textSegments, 1);
    EXPECT_NEAR(preview.textSeconds, 1.4, 0.01);
    EXPECT_EQ(preview.copiedSegments, 1);

    const fs::path out = dir_ / "scritta.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.textSegments, 1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.inside.size(), static_cast<std::size_t>(kFrames));
    EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, 4.0, 0.05);
    for (int i = 0; i < kFrames; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        EXPECT_NEAR(d.outside[static_cast<std::size_t>(i)], 100, 6);  // fuori dalla scritta il video non cambia
        if (i >= 38 && i <= 75)
            EXPECT_GT(d.inside[static_cast<std::size_t>(i)], 200);  // bianco (TV: 235)
        else if (i <= 34 || i >= 80)
            EXPECT_NEAR(d.inside[static_cast<std::size_t>(i)], 100, 6);
    }
}

TEST_F(TextExport, ManyTextsAreEachDrawnOnceAndFreedAfterwards)
{
    // Tante scritte una dopo l'altra: ognuna si disegna una volta sola (le immagini già usate si liberano senza
    // doverle rifare) e l'esportazione resta corretta.
    TextTracks tracks;
    const int layer = tracks.addLayer();
    constexpr int kTexts = 6;
    for (int i = 0; i < kTexts; ++i) {
        TextClip clip;
        clip.position = 0.1 + i * 0.6;
        clip.length = 0.4;
        clip.text = "A";
        tracks.add(layer, clip);
    }
    const CountingBox renderer;
    ReencodeOptions options;
    options.texts = tracks;
    options.textRenderer = &renderer;
    const std::vector<ClipRanges> plan{{file("grigio.mp4"), {{0.0, 4.0}}}};
    const fs::path out = dir_ / "tante.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.textSegments, kTexts);
    EXPECT_EQ(renderer.renders, kTexts);
    EXPECT_EQ(sample(out).errors, 0);
}

TEST(TextBlender, KeepsTheRequestedPixelFormat)
{
    // Il formato di uscita è quello chiesto (per esempio a 10 bit), non sempre YUV 4:2:0 a 8 bit.
    constexpr int w = 64;
    constexpr int h = 36;
    for (const AVPixelFormat format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE}) {
        SCOPED_TRACE(av_get_pix_fmt_name(format));
        TextFrameBlender blender(w, h, format);
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
        for (std::size_t i = 3; i < bgra.size(); i += 4)
            bgra[i] = 255;
        AVFrame* props = av_frame_alloc();
        props->pts = 7;
        const AVFrame* out = blender.blendRaster(bgra.data(), w * 4, w, h, {}, props);
        EXPECT_EQ(out->format, format);
        EXPECT_EQ(out->width, w);
        EXPECT_EQ(out->pts, 7);
        av_frame_free(&props);
    }
}

TEST(TextBlender, TextsFollowTheOrderOfTheLevels)
{
    // Una scritta sta sopra i livelli sotto di lei e sotto quelli sopra: V1 < V2 < ... e le scritte di un livello vanno con lui.
    constexpr int w = 64;
    constexpr int h = 36;
    VideoFrame red;  // un pezzo di livello tutto rosso (BGRA)
    red.width = w;
    red.height = h;
    red.stride = w * 4;
    red.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < red.pixels.size(); i += 4) {
        red.pixels[i + 2] = 255;
        red.pixels[i + 3] = 255;
    }
    TextImage white;  // una scritta bianca che copre la metà sinistra
    white.width = w / 2;
    white.height = h;
    white.rgba.assign(static_cast<std::size_t>(white.width) * h * 4, 255);

    const auto lumaAt = [&](int textLevel) {
        TextFrameBlender blender(w, h, AV_PIX_FMT_YUV420P);
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
        for (std::size_t i = 3; i < bgra.size(); i += 4)
            bgra[i] = 255;
        AVFrame* props = av_frame_alloc();
        blender.setLayers({{&red, Transform{}, 1}});
        const AVFrame* out = blender.blendRaster(bgra.data(), w * 4, w, h, {{&white, 1.0, textLevel}}, props);
        const int y = out->data[0][10 * out->linesize[0] + 10];
        av_frame_free(&props);
        return y;
    };
    const int under = lumaAt(0);  // una scritta di V1: sotto il livello V2, che la copre
    const int over = lumaAt(1);   // una scritta di V2 (lo stesso livello del pezzo): sopra
    const int above = lumaAt(2);  // una scritta di V3: sopra
    EXPECT_LT(under, 150);
    EXPECT_GT(under, 40);  // rosso, non nero
    EXPECT_GT(over, 200);
    EXPECT_GT(above, 200);
}

TEST_F(TextExport, WithoutARendererTheTextIsLeftOut)
{
    ReencodeOptions options;
    options.texts = oneText(1.2, 1.4);
    const std::vector<ClipRanges> plan{{file("grigio.mp4"), {{0.0, 4.0}}}};
    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, options);
    EXPECT_EQ(preview.textSegments, 0);
}

TEST_F(TextExport, TextOverAnEmptyGapIsDrawnOnTheBlack)
{
    const WhiteBox renderer;
    ReencodeOptions options;
    options.texts = oneText(0.2, 0.6);  // dentro lo spazio vuoto di 1 s prima del tratto
    options.textRenderer = &renderer;
    ClipRanges clip{file("grigio.mp4"), {{0.0, 1.0}}};
    clip.gapsBefore = {1.0};
    const fs::path out = dir_ / "scritta_nel_nero.mp4";
    const ExportResult result = exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.blackGaps, 1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.inside.size(), static_cast<std::size_t>(2 * kFps));
    for (int i = 0; i < 2 * kFps; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        if (i >= 8 && i <= 22)
            EXPECT_GT(d.inside[static_cast<std::size_t>(i)], 200);
        else if (i <= 4)
            EXPECT_NEAR(d.inside[static_cast<std::size_t>(i)], 16, 6);  // nero (TV: 16)
        else if (i >= 26 && i < kFps)
            EXPECT_NEAR(d.inside[static_cast<std::size_t>(i)], 16, 6);
        else if (i >= kFps + 2)
            EXPECT_NEAR(d.inside[static_cast<std::size_t>(i)], 100, 6);
    }
}

TEST_F(TextExport, FadesDimTheTextInAndOut)
{
    const WhiteBox renderer;
    ReencodeOptions options;
    options.texts = oneText(1.2, 1.4);  // fotogrammi 36 .. 77, dissolvenze di 0,7 s: piena a 1,9 s (fotogramma 57)
    options.texts.setFades(options.texts.layers()[0].clips[0].id, 0.7, 0.7);
    options.textRenderer = &renderer;
    const std::vector<ClipRanges> plan{{file("grigio.mp4"), {{0.0, 4.0}}}};
    const fs::path out = dir_ / "dissolvenze.mp4";
    exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.inside.size(), static_cast<std::size_t>(kFrames));
    const auto at = [&](int i) { return d.inside[static_cast<std::size_t>(i)]; };
    EXPECT_NEAR(at(33), 100, 6);
    EXPECT_GT(at(57), 200);                 // piena
    EXPECT_GT(at(46), 130);                 // a metà dissolvenza in entrata: né grigio né bianco
    EXPECT_LT(at(46), 200);
    EXPECT_GT(at(68), 130);                 // e in uscita
    EXPECT_LT(at(68), 200);
    EXPECT_LT(at(40), at(46));              // sale
    EXPECT_GT(at(62), at(72));              // poi scende
}

TEST_F(TextExport, AnimatedOpacityAndShiftApplyToTheDrawnText)
{
    const WhiteBox renderer;
    const std::vector<ClipRanges> plan{{file("grigio.mp4"), {{0.0, 4.0}}}};
    {
        // Opacità animata da 0 a 1 lungo la scritta (1,2 s → 2,6 s): grigio all'inizio, bianco alla fine.
        ReencodeOptions options;
        options.texts = oneText(1.2, 1.4);
        TransformAnimation anim;
        AnimKey a;
        a.time = 0.0;
        a.value = 0.0;
        AnimKey b;
        b.time = 1.4;
        b.value = 1.0;
        anim.track(AnimParam::Opacity) = {a, b};
        options.texts.setAnimation(options.texts.layers()[0].clips[0].id, anim);
        options.textRenderer = &renderer;
        const fs::path out = dir_ / "opacita-animata.mp4";
        exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
        const Sampled d = sample(out);
        EXPECT_EQ(d.errors, 0);
        ASSERT_EQ(d.inside.size(), static_cast<std::size_t>(kFrames));
        const auto at = [&](int i) { return d.inside[static_cast<std::size_t>(i)]; };
        EXPECT_NEAR(at(37), 100, 12);  // appena iniziata: quasi invisibile
        EXPECT_GT(at(57), 130);        // a metà: né grigio né bianco
        EXPECT_LT(at(57), 200);
        EXPECT_GT(at(76), 200);        // alla fine: bianca
        EXPECT_LT(at(40), at(57));
        EXPECT_LT(at(57), at(72));
    }
    {
        // Spostata di 300 pixel a destra, la scritta non sta più sopra il punto controllato.
        ReencodeOptions options;
        options.texts = oneText(1.2, 1.4);
        Transform t;
        t.posX = 300.0;
        options.texts.setTransform(options.texts.layers()[0].clips[0].id, t);
        options.textRenderer = &renderer;
        const fs::path out = dir_ / "spostata.mp4";
        exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
        const Sampled d = sample(out);
        EXPECT_EQ(d.errors, 0);
        ASSERT_EQ(d.inside.size(), static_cast<std::size_t>(kFrames));
        for (int i = 38; i <= 75; ++i)
            EXPECT_NEAR(d.inside[static_cast<std::size_t>(i)], 100, 6) << "fotogramma " << i;
    }
}
