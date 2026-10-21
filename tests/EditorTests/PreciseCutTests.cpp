#include "EditorCore/Compatibility.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "MediaChecks.h"

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

// File di prova H.264 (con la scheda video): 4 s a 30 fotogrammi al secondo, un fotogramma chiave al secondo
// (0, 1, 2, 3 s), luminanza uniforme 40 + numero del fotogramma, audio AAC.
constexpr int kFps = 30;
constexpr int kFrames = 120;

int lumaOf(int index)
{
    return 40 + index;
}

bool writeClip(const fs::path& path, int bFrames)
{
    try {
        VideoEncoderSettings s;
        s.width = 640;
        s.height = 360;
        s.frameRate = {kFps, 1};
        s.gopFrames = kFps;
        s.bFrames = bFrames;
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
    std::vector<int> luma;  // al centro, per ogni fotogramma in ordine di visualizzazione
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

// I fotogrammi da `first` (compreso) a `last` (escluso) del file di prova.
std::vector<int> frames(int first, int last)
{
    std::vector<int> out;
    for (int i = first; i < last; ++i)
        out.push_back(lumaOf(i));
    return out;
}

std::vector<int> concat(std::initializer_list<std::vector<int>> parts)
{
    std::vector<int> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Quanti fotogrammi hanno una luminanza diversa da quella attesa (o mancano).
int wrongFrames(const std::vector<int>& got, const std::vector<int>& expected)
{
    int wrong = static_cast<int>(std::max(got.size(), expected.size()) - std::min(got.size(), expected.size()));
    for (std::size_t i = 0; i < std::min(got.size(), expected.size()); ++i)
        wrong += std::abs(got[i] - expected[i]) > 3 ? 1 : 0;
    return wrong;
}

std::size_t rangeCount(const std::vector<ClipRanges>& plan)
{
    std::size_t count = 0;
    for (const ClipRanges& clip : plan)
        count += clip.ranges.size();
    return count;
}

class PreciseCut : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoTaglioPreciso";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeClip(dir_ / "senza_b.mp4", 0);
        // Con i B-frame (se l'encoder li fa davvero): per la fine del tratto.
        bFrames_ = ok_ && writeClip(dir_ / "con_b.mp4", 2)
                && readMediaSignature(utf8FromPath(dir_ / "con_b.mp4")).streams[0].bFrames;
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove del taglio preciso si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    // Esporta e controlla: fotogrammi attesi, nessun errore, audio lungo quanto il video e senza sovrapposizioni.
    static void check(const std::vector<ClipRanges>& plan, const std::vector<int>& expected, int preciseCuts,
                      const char* name)
    {
        for (const char* extension : {".mp4", ".mkv"}) {
            SCOPED_TRACE(std::string(name) + extension);
            const fs::path out = dir_ / (std::string(name) + extension);
            const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{});
            EXPECT_EQ(result.preciseCuts, preciseCuts);
            const Decoded decoded = decodeAll(out);
            EXPECT_EQ(decoded.errors, 0);
            EXPECT_EQ(wrongFrames(decoded.luma, expected), 0)
                << "primo fotogramma " << (decoded.luma.empty() ? -1 : decoded.luma.front()) << ", atteso "
                << (expected.empty() ? -1 : expected.front()) << "; fotogrammi " << decoded.luma.size() << " su "
                << expected.size();
            const double seconds = static_cast<double>(expected.size()) / kFps;
            EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, seconds, 0.05);
            const editor::tests::AudioTimeline audio = editor::tests::audioTimeline(utf8FromPath(out));
            // L'audio compresso si taglia a blocchi (AAC: ~21 ms): al più un blocco perso a ogni bordo esterno dei tratti (i
            // bordi del taglio preciso, dentro un tratto, non perdono niente).
            EXPECT_NEAR(audio.totalSeconds, seconds, 0.022 * 2 * static_cast<double>(rangeCount(plan)) + 0.005);
            EXPECT_FALSE(audio.overlaps);
        }
    }

    static inline fs::path dir_;
    static inline bool ok_ = false;
    static inline bool bFrames_ = false;
};

TEST_F(PreciseCut, InBetweenKeyframesStartsAtTheChosenFrame)
{
    // IN a 1,5 s (fotogramma 45), fra i fotogrammi chiave 1 e 2: si ricodifica 1,5 .. 2, poi si copia 2 .. 3.
    const std::vector<ClipRanges> plan{{file("senza_b.mp4"), {{1.5, 3.0}}}};
    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr);
    EXPECT_EQ(preview.preciseCuts, 1);
    EXPECT_NEAR(preview.preciseSeconds, 0.5, 1e-9);
    EXPECT_TRUE(preview.impreciseCuts.empty());
    check(plan, frames(45, 90), 1, "in_preciso");
}

TEST_F(PreciseCut, InAndOutInTheSameGopAreReencodedWhole)
{
    check({{file("senza_b.mp4"), {{1.2, 1.8}}}}, frames(36, 54), 1, "stesso_gop");
}

TEST_F(PreciseCut, OutWithBFramesReencodesTheLastPart)
{
    if (!bFrames_)
        GTEST_SKIP() << "L'encoder non ha fatto un file con i B-frame.";
    // OUT a 2,5 s: si copia 1 .. 2 e si ricodifica 2 .. 2,5 (i B-frame dopo il fotogramma chiave guardano avanti).
    check({{file("con_b.mp4"), {{1.0, 2.5}}}}, frames(30, 75), 1, "out_con_b");
}

TEST_F(PreciseCut, JoinsKeepFramesAndSound)
{
    // Due tagli precisi in un montaggio, anche dello stesso file all'indietro.
    const std::vector<ClipRanges> plan{{file("senza_b.mp4"), {{1.5, 3.0}, {0.2, 1.3}}}};
    check(plan, concat({frames(45, 90), frames(6, 39)}), 2, "montaggio");
}

}  // namespace
