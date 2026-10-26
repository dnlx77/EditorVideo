#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/TextImage.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;

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

bool writeClip(const fs::path& path)
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
                std::fill_n(frame->data[0] + y * frame->linesize[0], kWidth, static_cast<std::uint8_t>(100));
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
    std::vector<int> inside;   // luminanza dentro il rettangolo della scritta, per fotogramma
    std::vector<int> outside;  // luminanza fuori
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
    const auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            d.inside.push_back(frame->data[0][(kBoxY + kBoxH / 2) * frame->linesize[0] + kBoxX + kBoxW / 2]);
            d.outside.push_back(frame->data[0][20 * frame->linesize[0] + 20]);
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
