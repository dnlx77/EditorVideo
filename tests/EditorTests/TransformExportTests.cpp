#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Transform.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

// Trasformazioni nell'esportazione (step TR4). File di prova H.264: 4 s a 30 fotogrammi al secondo, un fotogramma chiave al
// secondo, luminanza uniforme 100, audio AAC. 
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kWidth = 640;
constexpr int kHeight = 360;

bool writeClip(const fs::path& path, int luma = 100)
{
    try {
        VideoEncoderSettings s;
        s.width = kWidth;
        s.height = kHeight;
        s.frameRate = {kFps, 1};
        s.gopFrames = kFps;
        s.bFrames = 0;
        VideoEncoder video(s);
        AudioEncoder audio({48000, 2});
        AVFormatContext* fmt = nullptr;
        const std::string name = utf8FromPath(path);
        avformat_alloc_output_context2(&fmt, nullptr, nullptr, name.c_str());
        AVStream* vs = avformat_new_stream(fmt, nullptr);
        video.copyParameters(vs->codecpar);
        vs->time_base = {1, kFps};
        AVStream* as = avformat_new_stream(fmt, nullptr);
        audio.copyParameters(as->codecpar);
        as->time_base = {1, 48000};
        avio_open(&fmt->pb, name.c_str(), AVIO_FLAG_WRITE);
        if (avformat_write_header(fmt, nullptr) < 0)
            return false;
        const auto sinkFor = [&](AVStream* st, AVRational tb) {
            return PacketSink([=](AVPacket* p) {
                AVPacket* copy = av_packet_clone(p);
                av_packet_rescale_ts(copy, tb, st->time_base);
                copy->stream_index = st->index;
                av_interleaved_write_frame(fmt, copy);
                av_packet_free(&copy);
            });
        };
        const PacketSink videoSink = sinkFor(vs, {1, kFps});
        const PacketSink audioSink = sinkFor(as, {1, 48000});
        for (int i = 0; i < kFrames; ++i) {
            AVFrame* frame = av_frame_alloc();
            frame->format = AV_PIX_FMT_YUV420P;
            frame->width = kWidth;
            frame->height = kHeight;
            av_frame_get_buffer(frame, 0);
            for (int y = 0; y < kHeight; ++y)
                std::fill_n(frame->data[0] + y * frame->linesize[0], kWidth, static_cast<std::uint8_t>(luma));
            for (int p = 1; p <= 2; ++p) {
                for (int y = 0; y < kHeight / 2; ++y)
                    std::fill_n(frame->data[p] + y * frame->linesize[p], kWidth / 2, 128);
            }
            frame->pts = i;
            video.encode(frame, false, videoSink);
            av_frame_free(&frame);
            AVFrame* a = av_frame_alloc();
            a->format = AV_SAMPLE_FMT_S16;
            a->sample_rate = 48000;
            a->nb_samples = 48000 / kFps;
            av_channel_layout_default(&a->ch_layout, 2);
            av_frame_get_buffer(a, 0);
            auto* s16 = reinterpret_cast<std::int16_t*>(a->data[0]);
            for (int n = 0; n < a->nb_samples; ++n)
                s16[2 * n] = s16[2 * n + 1] = static_cast<std::int16_t>(10000 * std::sin(2 * 3.14159265 * 440 * (i * a->nb_samples + n) / 48000.0));
            audio.encode(a, audioSink);
            av_frame_free(&a);
        }
        video.finish(videoSink);
        audio.finish(audioSink);
        av_write_trailer(fmt);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return true;
    } catch (const MediaError&) {
        return false;
    }
}

struct Sampled
{
    std::vector<std::array<int, 3>> pixels;  // per fotogramma: centro, angolo in alto a sinistra, centro del quarto destro
    int errors = 0;
};

Sampled sample(const fs::path& path)
{
    Sampled d;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8FromPath(path).c_str(), nullptr, nullptr) < 0) {
        d.errors = 1;
        return d;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[video]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[video]->codecpar);
    avcodec_open2(ctx, codec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto at = [&](int x, int y) { return static_cast<int>(frame->data[0][y * frame->linesize[0] + x]); };
    const auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            d.pixels.push_back({at(kWidth / 2, kHeight / 2), at(20, 20), at(kWidth * 3 / 4 + 40, kHeight / 2)});
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video) {
            if (avcodec_send_packet(ctx, pkt) < 0)
                ++d.errors;
            drain();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
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
