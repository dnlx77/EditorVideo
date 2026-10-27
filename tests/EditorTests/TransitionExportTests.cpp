#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"

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

// Transizioni nell'esportazione (step X8). Due file H.264 di 4 s a 30 fotogrammi al secondo, un fotogramma chiave al
// secondo, luminanza uniforme (60 il primo, 200 il secondo), audio AAC.
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kWidth = 640;
constexpr int kHeight = 360;

bool writeClip(const fs::path& path, std::uint8_t luma)
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
                std::fill_n(frame->data[0] + y * frame->linesize[0], kWidth, luma);
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


// La luminanza al centro di ogni fotogramma del file.
std::vector<int> centerLuma(const fs::path& path, int* errors)
{
    std::vector<int> result;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8FromPath(path).c_str(), nullptr, nullptr) < 0) {
        ++*errors;
        return result;
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
            result.push_back(frame->data[0][(kHeight / 2) * frame->linesize[0] + kWidth / 2]);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video) {
            if (avcodec_send_packet(ctx, pkt) < 0)
                ++*errors;
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
