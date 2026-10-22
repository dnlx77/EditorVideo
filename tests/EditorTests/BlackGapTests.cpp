#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

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
using editor::tests::SampleDirTest;

namespace {

// File di prova H.264 (con la scheda video): 4 s a 30 fotogrammi al secondo, un fotogramma chiave al secondo,
// luminanza uniforme 40 + numero del fotogramma, audio AAC.
constexpr int kFps = 30;
constexpr int kFrames = 120;
constexpr int kBlack = -1;  // nei fotogrammi attesi: nero

int lumaOf(int index)
{
    return 40 + index;
}

bool writeClip(const fs::path& path)
{
    try {
        VideoEncoderSettings s;
        s.width = 640;
        s.height = 360;
        s.frameRate = {kFps, 1};
        s.gopFrames = kFps;
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
            frame->width = 640;
            frame->height = 360;
            av_frame_get_buffer(frame, 0);
            for (int y = 0; y < 360; ++y)
                std::fill_n(frame->data[0] + y * frame->linesize[0], 640, static_cast<std::uint8_t>(lumaOf(i)));
            for (int p = 1; p <= 2; ++p) {
                for (int y = 0; y < 180; ++y)
                    std::fill_n(frame->data[p] + y * frame->linesize[p], 320, 128);
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

struct Decoded
{
    std::vector<int> luma;
    int errors = 0;
};

Decoded decodeAll(const fs::path& path)
{
    Decoded d;
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
    ctx->err_recognition = AV_EF_EXPLODE | AV_EF_BITSTREAM;
    avcodec_open2(ctx, codec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            d.luma.push_back(frame->data[0][180 * frame->linesize[0] + 320]);
            d.errors += (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) ? 1 : 0;
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

// Fotogrammi attesi: neri, oppure dal fotogramma `first` (compreso) a `last` (escluso) del file di prova.
void addBlack(std::vector<int>& out, int count)
{
    out.insert(out.end(), static_cast<std::size_t>(count), kBlack);
}

void addFrames(std::vector<int>& out, int first, int last)
{
    for (int i = first; i < last; ++i)
        out.push_back(lumaOf(i));
}

int wrongFrames(const std::vector<int>& got, const std::vector<int>& expected)
{
    int wrong = static_cast<int>(std::max(got.size(), expected.size()) - std::min(got.size(), expected.size()));
    for (std::size_t i = 0; i < std::min(got.size(), expected.size()); ++i) {
        const bool ok = expected[i] == kBlack ? got[i] <= 20 : std::abs(got[i] - expected[i]) <= 3;
        wrong += ok ? 0 : 1;
    }
    return wrong;
}

class BlackGaps : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoSpaziNeri";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        ok_ = preferredVideoEncoder(VideoCodec::H264) && writeClip(dir_ / "clip.mp4");
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove degli spazi neri si saltano.";
    }

    static std::string clip() { return utf8FromPath(dir_ / "clip.mp4"); }

    static inline fs::path dir_;
    static inline bool ok_ = false;
};

TEST_F(BlackGaps, GapsBecomeBlackAndSilenceAsLongAsInTheMontage)
{
    // Spazio di 0,5 s all'inizio, 0..1, spazio di 1,2 s, 2..3, spazio di 0,1 s (3 fotogrammi), 1..2.
    const std::vector<ClipRanges> plan{{clip(), {{0.0, 1.0}, {2.0, 3.0}, {1.0, 2.0}}, {}, {0.5, 1.2, 0.1}}};
    std::vector<int> expected;
    addBlack(expected, 15);
    addFrames(expected, 0, 30);
    addBlack(expected, 36);
    addFrames(expected, 60, 90);
    addBlack(expected, 3);
    addFrames(expected, 30, 60);
    const double total = 4.8;

    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr);
    EXPECT_EQ(preview.blackGaps, 3);
    EXPECT_NEAR(preview.blackSeconds, 1.8, 1e-9);
    EXPECT_NEAR(preview.outputSeconds, total, 1e-9);
    EXPECT_TRUE(preview.closedGaps.empty()) << preview.closedGaps;

    for (const char* extension : {".mp4", ".mkv"}) {
        SCOPED_TRACE(extension);
        const fs::path out = dir_ / (std::string("spazi") + extension);
        const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{});
        EXPECT_EQ(result.blackGaps, 3);
        const Decoded decoded = decodeAll(out);
        EXPECT_EQ(decoded.errors, 0);
        EXPECT_EQ(wrongFrames(decoded.luma, expected), 0) << "fotogrammi " << decoded.luma.size() << " su " << expected.size();
        EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, total, 0.05);
        // Audio lungo quanto il video (al più un blocco AAC perso a ogni bordo esterno dei tratti) e senza sovrapposizioni.
        const editor::tests::AudioTimeline audio = editor::tests::audioTimeline(utf8FromPath(out));
        EXPECT_NEAR(audio.totalSeconds, total, 0.022 * 6 + 0.005);
        EXPECT_FALSE(audio.overlaps);
    }
}

TEST_F(BlackGaps, CloseGapsKeepsThePiecesTogether)
{
    const std::vector<ClipRanges> plan{{clip(), {{0.0, 1.0}, {2.0, 3.0}}, {}, {0.5, 1.2}}};
    const ReencodeOptions close{QualityPreset::High, false, true};
    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr, close);
    EXPECT_EQ(preview.blackGaps, 0);
    EXPECT_NEAR(preview.outputSeconds, 2.0, 1e-9);
    const fs::path out = dir_ / "chiusi.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, close);
    EXPECT_EQ(result.blackGaps, 0);
    std::vector<int> expected;
    addFrames(expected, 0, 30);
    addFrames(expected, 60, 90);
    const Decoded decoded = decodeAll(out);
    EXPECT_EQ(decoded.errors, 0);
    EXPECT_EQ(wrongFrames(decoded.luma, expected), 0);
}

TEST_F(BlackGaps, AGapShorterThanHalfAFrameLeavesNothing)
{
    const std::vector<ClipRanges> plan{{clip(), {{0.0, 1.0}, {2.0, 3.0}}, {}, {0.0, 0.01}}};
    const fs::path out = dir_ / "corto.mkv";
    exportClips(plan, utf8FromPath(out), AudioEdit{});
    std::vector<int> expected;
    addFrames(expected, 0, 30);
    addFrames(expected, 60, 90);
    const Decoded decoded = decodeAll(out);
    EXPECT_EQ(decoded.errors, 0);
    EXPECT_EQ(wrongFrames(decoded.luma, expected), 0);
}

// Senza ricodifica possibile (file MPEG-4 Part 2): gli spazi si chiudono e il riepilogo lo dice. Non serve la scheda
// video: questa prova gira anche nella build automatica.
class ClosedGapsTest : public SampleDirTest<ClosedGapsTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoSpaziChiusi";
    static void prepareSamples() { editor::tests::writeExportSources(dir_); }
};

TEST_F(ClosedGapsTest, WithoutAnEncoderForTheFormatGapsAreClosed)
{
    // sorgente.mkv: 4 s a 25 fotogrammi al secondo, un fotogramma chiave al secondo.
    const std::vector<ClipRanges> plan{{path("sorgente.mkv"), {{0.0, 1.0}, {2.0, 3.0}}, {}, {0.4, 1.0}}};
    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr);
    EXPECT_EQ(preview.blackGaps, 0);
    EXPECT_FALSE(preview.closedGaps.empty());
    EXPECT_NEAR(preview.outputSeconds, 2.0, 1e-9);
    const std::string out = path("chiusi.mkv");
    const ExportResult result = exportClips(plan, out, AudioEdit{});
    EXPECT_EQ(result.blackGaps, 0);
    EXPECT_NEAR(probeFile(out).durationSeconds, 2.0, 0.1);
}

}  // namespace
