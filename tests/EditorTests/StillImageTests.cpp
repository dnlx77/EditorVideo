#include "EditorCore/FileDrop.h"
#include "EditorCore/LayerPlayback.h"
#include "EditorCore/VideoDecoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/StillImage.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

struct Rgba
{
    unsigned char r, g, b, a;
};

// Il colore del pixel (x, y) delle immagini di prova: dipende dalla posizione, così un'immagine ruotata o specchiata si riconosce.
Rgba pixelAt(int x, int y)
{
    return {static_cast<unsigned char>(x * 20), static_cast<unsigned char>(y * 20), 90,
            static_cast<unsigned char>((x + y) % 2 == 0 ? 255 : 128)};
}

// Scrive un'immagine di prova w x h con l'encoder indicato (PNG con RGBA, BMP con BGRA). False se manca l'encoder.
bool writeImage(const fs::path& path, int w, int h, AVCodecID id, AVPixelFormat format)
{
    const AVCodec* codec = avcodec_find_encoder(id);
    if (codec == nullptr)
        return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = w;
    ctx->height = h;
    ctx->pix_fmt = format;
    ctx->time_base = {1, 25};
    bool ok = avcodec_open2(ctx, codec, nullptr) >= 0;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (ok) {
        frame->format = format;
        frame->width = w;
        frame->height = h;
        ok = av_frame_get_buffer(frame, 0) >= 0;
    }
    if (ok) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const Rgba c = pixelAt(x, y);
                unsigned char* p = frame->data[0] + y * frame->linesize[0] + x * 4;
                if (format == AV_PIX_FMT_RGBA) {
                    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
                } else {
                    p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = c.a;
                }
            }
        }
        frame->pts = 0;
        ok = avcodec_send_frame(ctx, frame) >= 0 && avcodec_send_frame(ctx, nullptr) >= 0 &&
             avcodec_receive_packet(ctx, packet) >= 0;
    }
    if (ok) {
        std::FILE* file = fs::exists(path) ? nullptr : std::fopen(path.string().c_str(), "wb");
        if (file != nullptr) {
            ok = std::fwrite(packet->data, 1, static_cast<std::size_t>(packet->size), file) == static_cast<std::size_t>(packet->size);
            std::fclose(file);
        } else {
            ok = false;
        }
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

class StillImageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() / "EditorVideoStillImageTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        havePng_ = writeImage(dir_ / "prova.png", 8, 6, AV_CODEC_ID_PNG, AV_PIX_FMT_RGBA);
        haveBmp_ = writeImage(dir_ / "prova.bmp", 8, 6, AV_CODEC_ID_BMP, AV_PIX_FMT_BGRA);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    std::string path(const std::string& name) const { return utf8FromPath(dir_ / name); }

    fs::path dir_;
    bool havePng_ = false;
    bool haveBmp_ = false;
};

TEST(StillImageExtensions, RecognizesImagesByExtension)
{
    EXPECT_TRUE(isImagePath("C:\\foto\\mare.PNG"));
    EXPECT_TRUE(isImagePath("a.jpeg"));
    EXPECT_TRUE(isImagePath("a.WebP"));
    EXPECT_FALSE(isImagePath("film.mp4"));
    EXPECT_FALSE(isImagePath("senza_estensione"));
    const auto& media = mediaExtensions();
    EXPECT_NE(std::find(media.begin(), media.end(), ".png"), media.end());
    EXPECT_NE(std::find(media.begin(), media.end(), ".mp4"), media.end());
}

TEST_F(StillImageTest, LoadsPngWithPixelsAndTransparency)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    const auto frame = loadImage(path("prova.png"));
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->width, 8);
    EXPECT_EQ(frame->height, 6);
    EXPECT_EQ(frame->stride, 8 * 4);
    EXPECT_EQ(frame->time, 0.0);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) {
            const Rgba c = pixelAt(x, y);
            const unsigned char* p = frame->pixels.data() + y * frame->stride + x * 4;  // B G R A
            ASSERT_EQ(p[0], c.b) << x << "," << y;
            ASSERT_EQ(p[1], c.g) << x << "," << y;
            ASSERT_EQ(p[2], c.r) << x << "," << y;
            ASSERT_EQ(p[3], c.a) << x << "," << y;
        }
    }
}

TEST_F(StillImageTest, ShrinksToBoxButNeverEnlarges)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    const auto small = loadImage(path("prova.png"), 4, 4);
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(small->width, 4);
    EXPECT_EQ(small->height, 3);
    const auto same = loadImage(path("prova.png"), 100, 100);
    ASSERT_NE(same, nullptr);
    EXPECT_EQ(same->width, 8);
    EXPECT_EQ(same->height, 6);
}

TEST_F(StillImageTest, InspectsSizeAndRejectsNonImages)
{
    if (!haveBmp_)
        GTEST_SKIP() << "encoder BMP non disponibile";
    const auto size = inspectImage(path("prova.bmp"));
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size->width, 8);
    EXPECT_EQ(size->height, 6);
    EXPECT_FALSE(inspectImage(path("manca.png")).has_value());
    EXPECT_EQ(loadImage(path("manca.png")), nullptr);
    // Un file che ha l'estensione ma non è un'immagine.
    std::FILE* f = std::fopen(path("finta.png").c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("non sono un'immagine", f);
    std::fclose(f);
    EXPECT_FALSE(inspectImage(path("finta.png")).has_value());
}

TEST_F(StillImageTest, ProbeGivesStillDurationAndNoKeyframes)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    const MediaInfo info = probeFile(path("prova.png"));
    EXPECT_EQ(info.durationSeconds, kStillDuration);
    EXPECT_TRUE(info.keyframeTimes.empty());
    ASSERT_GE(info.keyframeStreamIndex, 0);
}

TEST_F(StillImageTest, ProjectMarksImageClipsAsStill)
{
    Project project;
    const int image = project.add(path("prova.png"));
    const int video = project.add(path("film.mp4"));
    ASSERT_NE(project.find(image), nullptr);
    EXPECT_TRUE(project.find(image)->still);
    EXPECT_FALSE(project.find(video)->still);
    project.setClipPath(video, path("altra.JPG"));
    EXPECT_TRUE(project.find(video)->still);
}

TEST_F(StillImageTest, DropPutsImageInSequenceWithDefaultLength)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    Project project;
    const DropPlan plan = planDrop({path("prova.png")}, project, inspectMedia);
    ASSERT_EQ(plan.media.size(), 1u);
    EXPECT_EQ(plan.media[0].duration, kStillDuration);
    EXPECT_TRUE(plan.unreadable.empty());
    std::vector<int> fragments;
    applyDrop(project, plan.media, 0, &fragments);
    ASSERT_EQ(fragments.size(), 1u);
    const Fragment* piece = project.sequence().find(fragments[0]);
    ASSERT_NE(piece, nullptr);
    // Parte da metà del "file": il pezzo si può allungare da tutte e due le parti.
    EXPECT_EQ(piece->start, kStillCenter);
    EXPECT_EQ(piece->end, kStillCenter + kStillDefaultLength);
}

TEST_F(StillImageTest, FolderDropIncludesImages)
{
    if (!havePng_ || !haveBmp_)
        GTEST_SKIP() << "encoder immagini non disponibili";
    Project project;
    const DropPlan plan = planDrop({utf8FromPath(dir_)}, project, inspectMedia);
    EXPECT_EQ(plan.media.size(), 2u);  // prova.bmp, prova.png
}

TEST_F(StillImageTest, DecoderGivesTheSameFrameAtAnyTime)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    VideoDecoder decoder(path("prova.png"), {.allowHardware = false});
    EXPECT_EQ(decoder.durationSeconds(), kStillDuration);
    EXPECT_GT(decoder.frameRate(), 0.0);
    EXPECT_EQ(decoder.outputWidth(), 8);
    EXPECT_EQ(decoder.outputHeight(), 6);
    const auto first = decoder.seek(0.0);
    const auto later = decoder.seek(12.34);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(later, nullptr);
    EXPECT_EQ(first->time, 0.0);
    EXPECT_DOUBLE_EQ(later->time, 12.34);
    EXPECT_EQ(first->pixels, later->pixels);
    // Il fotogramma dato non cambia se se ne chiede un altro (chi lo tiene non vede mutare i pixel).
    EXPECT_EQ(first->time, 0.0);
    // nextFrame avanza di un fotogramma alla volta.
    const auto next = decoder.nextFrame();
    ASSERT_NE(next, nullptr);
    EXPECT_NEAR(next->time, 12.34 + 1.0 / decoder.frameRate(), 1e-9);
    EXPECT_EQ(next->pixels, first->pixels);
}

TEST_F(StillImageTest, DecoderScalesToTheOutputBox)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    VideoDecoder decoder(path("prova.png"), {.allowHardware = false});
    decoder.setOutputSize(32, 24);
    const auto frame = decoder.seek(1.0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->width, 32);
    EXPECT_EQ(frame->height, 24);
    EXPECT_EQ(frame->pixels.size(), static_cast<std::size_t>(32 * 24 * 4));
}

TEST_F(StillImageTest, LayerPlaybackShowsTheImage)
{
    if (!havePng_)
        GTEST_SKIP() << "encoder PNG non disponibile";
    LayerPlayback playback(path("prova.png"), 8, 6, false);
    const auto frame = playback.waitFrame(3.0);
    ASSERT_NE(frame, nullptr);
    EXPECT_FALSE(playback.failed());
    EXPECT_EQ(frame->width, 8);
}

TEST_F(StillImageTest, FallbackLoaderReadsWhatFfmpegCannot)
{
    // Un file che FFmpeg non legge (non è un vero PNG) ma il lettore di riserva sì: come il PNG nella build di Windows.
    std::FILE* f = std::fopen(path("riserva.png").c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("dati che solo il lettore di riserva capisce", f);
    std::fclose(f);
    EXPECT_EQ(loadImage(path("riserva.png")), nullptr);
    EXPECT_FALSE(inspectImage(path("riserva.png")).has_value());

    setImageLoader([](const std::string&) {
        auto frame = std::make_shared<VideoFrame>();
        frame->width = 20;
        frame->height = 10;
        frame->stride = 80;
        frame->pixels.assign(800, 200);
        return frame;
    });
    const auto frame = loadImage(path("riserva.png"));
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->width, 20);
    const auto shrunk = loadImage(path("riserva.png"), 10, 10);
    ASSERT_NE(shrunk, nullptr);
    EXPECT_EQ(shrunk->width, 10);
    EXPECT_EQ(shrunk->height, 5);
    // La probe e il decodificatore passano dallo stesso lettore.
    const MediaInfo info = probeFile(path("riserva.png"));
    ASSERT_EQ(info.streams.size(), 1u);
    EXPECT_EQ(info.streams[0].width, 20);
    EXPECT_EQ(info.durationSeconds, kStillDuration);
    VideoDecoder decoder(path("riserva.png"), {.allowHardware = false});
    EXPECT_EQ(decoder.outputWidth(), 20);
    ASSERT_NE(decoder.seek(2.0), nullptr);
    setImageLoader(nullptr);
    EXPECT_EQ(loadImage(path("riserva.png")), nullptr);
}

TEST_F(StillImageTest, ImagesHaveNoAudioWhenMovedOrLinked)
{
    Project project;
    const int image = project.add(path("foto.jpg"));
    project.setAnalysis(image, {}, kStillDuration);
    const int fragment = project.sequence().insert(-1, image, kStillCenter, kStillCenter + 5.0);
    ASSERT_NE(fragment, 0);
    // Scollegare/ricollegare l'audio: niente da fare.
    EXPECT_EQ(project.unlinkAudio(fragment), 0);
    EXPECT_FALSE(project.relinkAudio(fragment));
    // Su un livello: nessuna clip audio e nessun gruppo con l'audio.
    const int layer = project.textTracks().addLayer();
    int audioClip = -1;
    const int piece = project.liftToLayer(fragment, layer, 1.0, &audioClip);
    ASSERT_NE(piece, 0);
    EXPECT_EQ(audioClip, 0);
    for (const AudioTrack& track : project.audioTracks().tracks())
        EXPECT_TRUE(track.clips.empty());
    // E di nuovo in V1: il pezzo non diventa "audio scollegato".
    const int back = project.lowerToV1(piece, 0.0, true, &audioClip);
    ASSERT_NE(back, 0);
    EXPECT_EQ(audioClip, 0);
    EXPECT_FALSE(project.sequence().find(back)->detached);
    EXPECT_TRUE(project.sequence().find(back)->muted.empty());
    // Pezzo messo direttamente su un livello con addLayerPiece(withAudio = true).
    LayerPiece lp;
    lp.clipId = image;
    lp.start = kStillCenter;
    lp.end = kStillCenter + 5.0;
    lp.position = 10.0;
    audioClip = -1;
    ASSERT_NE(project.addLayerPiece(layer, lp, true, &audioClip), 0);
    EXPECT_EQ(audioClip, 0);
}

TEST_F(StillImageTest, TrimmingAnImageCanGrowBothWays)
{
    Project project;
    const int image = project.add(path("foto.jpg"));
    project.setAnalysis(image, {}, kStillDuration);
    const int fragment = project.sequence().insert(-1, image, kStillCenter, kStillCenter + 5.0);
    ASSERT_NE(fragment, 0);
    EXPECT_TRUE(project.sequence().trim(fragment, Sequence::Edge::End, kStillCenter + 30.0, kStillDuration, 0.04, false));
    EXPECT_NEAR(project.sequence().find(fragment)->length(), 30.0, 1e-9);
    EXPECT_TRUE(project.sequence().trim(fragment, Sequence::Edge::Start, kStillCenter - 10.0, kStillDuration, 0.04, true));
    EXPECT_NEAR(project.sequence().find(fragment)->length(), 40.0, 1e-9);
}

TEST_F(StillImageTest, SignatureOfAnImageHasItsSize)
{
    if (!haveBmp_)
        GTEST_SKIP() << "encoder BMP non disponibile";
    const MediaSignature signature = readMediaSignature(path("prova.bmp"));
    const StreamSignature* video = findStream(signature.streams, StreamKind::Video);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->width, 8);
    EXPECT_EQ(video->height, 6);
    EXPECT_EQ(findStream(signature.streams, StreamKind::Audio), nullptr);
}

}  // namespace
