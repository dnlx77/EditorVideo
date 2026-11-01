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
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

// File di prova: ogni fotogramma ha una luminanza uniforme che dice da quale file e da quale fotogramma viene.
struct ClipSpec
{
    VideoCodec codec = VideoCodec::H264;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int frames = 120;  // 2 s
    int gop = 60;      // un fotogramma chiave al secondo
    int lumaBase = 40;
    int audioRate = 48000;  // 0 = senza audio
};

int lumaOf(const ClipSpec& spec, int index)
{
    return spec.lumaBase + index;
}

bool writeClip(const fs::path& path, const ClipSpec& spec)
{
    try {
        VideoEncoderSettings s;
        s.codec = spec.codec;
        s.width = spec.width;
        s.height = spec.height;
        s.frameRate = {spec.fps, 1};
        s.gopFrames = spec.gop;
        VideoEncoder video(s);
        std::unique_ptr<AudioEncoder> audio;
        if (spec.audioRate > 0)
            audio = std::make_unique<AudioEncoder>(AudioEncoderSettings{spec.audioRate, 2});
        AVFormatContext* fmt = nullptr;
        const std::string name = utf8FromPath(path);
        avformat_alloc_output_context2(&fmt, nullptr, nullptr, name.c_str());
        AVStream* vs = avformat_new_stream(fmt, nullptr);
        video.copyParameters(vs->codecpar);
        vs->time_base = {1, spec.fps};
        AVStream* as = nullptr;
        if (audio) {
            as = avformat_new_stream(fmt, nullptr);
            audio->copyParameters(as->codecpar);
            as->time_base = {1, spec.audioRate};
        }
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
        const PacketSink videoSink = sinkFor(vs, {1, spec.fps});
        const PacketSink audioSink = audio ? sinkFor(as, {1, spec.audioRate}) : PacketSink{};
        long long sample = 0;
        for (int i = 0; i < spec.frames; ++i) {
            AVFrame* frame = av_frame_alloc();
            frame->format = AV_PIX_FMT_YUV420P;
            frame->width = spec.width;
            frame->height = spec.height;
            av_frame_get_buffer(frame, 0);
            for (int y = 0; y < spec.height; ++y)
                std::fill_n(frame->data[0] + y * frame->linesize[0], spec.width, static_cast<std::uint8_t>(lumaOf(spec, i)));
            for (int p = 1; p <= 2; ++p) {
                for (int y = 0; y < spec.height / 2; ++y)
                    std::fill_n(frame->data[p] + y * frame->linesize[p], spec.width / 2, 128);
            }
            frame->pts = i;
            video.encode(frame, false, videoSink);
            av_frame_free(&frame);
            if (audio) {
                // Un La per tutto il file, fino alla fine di questo fotogramma.
                const long long until = static_cast<long long>(i + 1) * spec.audioRate / spec.fps;
                AVFrame* a = av_frame_alloc();
                a->format = AV_SAMPLE_FMT_S16;
                a->sample_rate = spec.audioRate;
                a->nb_samples = static_cast<int>(until - sample);
                av_channel_layout_default(&a->ch_layout, 2);
                av_frame_get_buffer(a, 0);
                auto* s16 = reinterpret_cast<std::int16_t*>(a->data[0]);
                for (int n = 0; n < a->nb_samples; ++n)
                    s16[2 * n] = s16[2 * n + 1] = static_cast<std::int16_t>(12000 * std::sin(2 * 3.14159265 * 440 * (sample + n) / spec.audioRate));
                sample = until;
                audio->encode(a, audioSink);
                av_frame_free(&a);
            }
        }
        video.finish(videoSink);
        if (audio)
            audio->finish(audioSink);
        av_write_trailer(fmt);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return true;
    } catch (const MediaError&) {
        return false;
    }
}

// Se il file ha un flusso di quel tipo.
bool findStreamOfKind(const MediaInfo& info, StreamKind kind)
{
    for (const StreamInfo& stream : info.streams) {
        if (stream.kind == kind)
            return true;
    }
    return false;
}

struct Decoded
{
    std::vector<int> centre;  // luminanza al centro di ogni fotogramma
    std::vector<int> edge;    // a 8 pixel dal bordo sinistro, a metà altezza
    int errors = 0;
    int width = 0;
    int height = 0;
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
    ctx->thread_count = 0;
    avcodec_open2(ctx, codec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            d.width = frame->width;
            d.height = frame->height;
            d.centre.push_back(frame->data[0][(frame->height / 2) * frame->linesize[0] + frame->width / 2]);
            d.edge.push_back(frame->data[0][(frame->height / 2) * frame->linesize[0] + 8]);
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

std::vector<std::vector<std::uint8_t>> videoPackets(const fs::path& path)
{
    std::vector<std::vector<std::uint8_t>> out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8FromPath(path).c_str(), nullptr, nullptr) < 0)
        return out;
    avformat_find_stream_info(fmt, nullptr);
    const int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video)
            out.emplace_back(pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return out;
}

// Il pacchetto `written` è `original`, con eventualmente davanti i parametri di codifica.
bool sameOrWithParameters(const std::vector<std::uint8_t>& written, const std::vector<std::uint8_t>& original)
{
    return written.size() >= original.size()
        && std::equal(original.begin(), original.end(), written.end() - static_cast<std::ptrdiff_t>(original.size()));
}

class Reencode : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoRicodifica";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        // Il progetto: H.264 1080p60 con AAC 48 kHz (il primo pezzo).
        ok_ = writeClip(dir_ / "a_1080p60.mp4", kA)
           && writeClip(dir_ / "b_720p30.mp4", kB)       // altra risoluzione e frequenza, stesso audio: audio copiato
           && writeClip(dir_ / "c_480p30_44k.mp4", kC);  // 4:3 (bande nere) e audio a 44,1 kHz (ricodificato)
        hevc_ = ok_ && preferredVideoEncoder(VideoCodec::Hevc) && writeClip(dir_ / "d_hevc_muto.mp4", kD);
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove della ricodifica si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    static inline const ClipSpec kA{VideoCodec::H264, 1920, 1080, 60, 120, 60, 40, 48000};
    static inline const ClipSpec kB{VideoCodec::H264, 1280, 720, 30, 60, 30, 100, 48000};
    static inline const ClipSpec kC{VideoCodec::H264, 640, 480, 30, 60, 30, 170, 44100};
    static inline const ClipSpec kD{VideoCodec::Hevc, 1920, 1080, 60, 120, 60, 60, 0};
    static inline fs::path dir_;
    static inline bool ok_ = false;
    static inline bool hevc_ = false;
};

TEST_F(Reencode, DifferentPiecesAreReencodedAndJoinedToCopiedOnes)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("a_1080p60.mp4")));
    std::vector<ClipRanges> plan{{file("a_1080p60.mp4"), {{0.0, 2.0}}},
                                 {file("b_720p30.mp4"), {{0.0, 2.0}}},
                                 {file("c_480p30_44k.mp4"), {{0.0, 2.0}}}};
    if (hevc_)
        plan.push_back({file("d_hevc_muto.mp4"), {{0.0, 2.0}}});
    plan.push_back({file("a_1080p60.mp4"), {{1.0, 2.0}}});
    const int reencoded = hevc_ ? 3 : 2;
    const double total = 2.0 * (plan.size() - 1) + 1.0;

    // Il riepilogo prima di esportare.
    const ExportPreview preview = previewExport(plan, AudioEdit{}, &format);
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;
    EXPECT_EQ(preview.reencodedSegments, reencoded);
    EXPECT_EQ(preview.copiedSegments, 2);
    EXPECT_NEAR(preview.reencodedSeconds, 2.0 * reencoded, 1e-9);

    // Fotogrammi attesi (luminanza al centro): a 60 al secondo, quelli a 30 ripetuti due volte.
    std::vector<int> expected;
    for (int i = 0; i < 120; ++i)
        expected.push_back(lumaOf(kA, i));
    for (int k = 0; k < 120; ++k)
        expected.push_back(lumaOf(kB, k / 2));
    for (int k = 0; k < 120; ++k)
        expected.push_back(lumaOf(kC, k / 2));
    if (hevc_) {
        for (int i = 0; i < 120; ++i)
            expected.push_back(lumaOf(kD, i));
    }
    for (int i = 60; i < 120; ++i)
        expected.push_back(lumaOf(kA, i));
    const std::vector<std::vector<std::uint8_t>> source = videoPackets(dir_ / "a_1080p60.mp4");

    for (const char* extension : {".mp4", ".mkv"}) {
        SCOPED_TRACE(extension);
        const fs::path out = dir_ / (std::string("montaggio") + extension);
        const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format);
        EXPECT_EQ(result.reencodedSegments, reencoded);
        EXPECT_TRUE(result.parametersInStream);

        // Durata, e audio lungo quanto il video, continuo (le giunzioni allineate).
        const MediaInfo info = probeFile(utf8FromPath(out));
        EXPECT_NEAR(info.durationSeconds, total, 0.1);
        const editor::tests::AudioTimeline audio = editor::tests::audioTimeline(utf8FromPath(out));
        EXPECT_NEAR(audio.totalSeconds, total, 0.15);
        EXPECT_FALSE(audio.overlaps);

        // Tutti i fotogrammi, senza errori, nell'ordine giusto e a 1920x1080.
        const Decoded decoded = decodeAll(out);
        EXPECT_EQ(decoded.errors, 0);
        EXPECT_EQ(decoded.width, 1920);
        EXPECT_EQ(decoded.height, 1080);
        ASSERT_EQ(decoded.centre.size(), expected.size());
        int wrong = 0;
        for (std::size_t i = 0; i < expected.size(); ++i)
            wrong += std::abs(decoded.centre[i] - expected[i]) > 3 ? 1 : 0;
        EXPECT_EQ(wrong, 0) << "fotogrammi diversi da quelli attesi";
        // Il 640x480 (4:3) ha le bande nere ai lati.
        for (std::size_t i = 240; i < 360; ++i)
            EXPECT_LE(decoded.edge[i], 20) << "fotogramma " << i;

        // I pezzi copiati: pacchetti identici agli originali (davanti ai fotogrammi chiave i parametri).
        const std::vector<std::vector<std::uint8_t>> written = videoPackets(out);
        ASSERT_EQ(written.size(), expected.size());
        int different = 0;
        for (std::size_t i = 0; i < 120; ++i)
            different += sameOrWithParameters(written[i], source[i]) ? 0 : 1;
        for (std::size_t i = 0; i < 60; ++i)
            different += sameOrWithParameters(written[written.size() - 60 + i], source[60 + i]) ? 0 : 1;
        EXPECT_EQ(different, 0);
    }
}

TEST_F(Reencode, ReencodeEverythingAndCancel)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("a_1080p60.mp4")));
    const std::vector<ClipRanges> plan{{file("a_1080p60.mp4"), {{0.0, 2.0}}}, {file("b_720p30.mp4"), {{0.0, 1.0}}}};

    // "Ricodifica tutto": anche il pezzo uguale al formato.
    const fs::path out = dir_ / "tutto.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format,
                                            ReencodeOptions{QualityPreset::Medium, true});
    EXPECT_EQ(result.reencodedSegments, 2);
    const Decoded decoded = decodeAll(out);
    EXPECT_EQ(decoded.errors, 0);
    EXPECT_EQ(decoded.centre.size(), 180u);
    const std::vector<std::vector<std::uint8_t>> source = videoPackets(dir_ / "a_1080p60.mp4");
    const std::vector<std::vector<std::uint8_t>> written = videoPackets(out);
    ASSERT_GE(written.size(), 2u);
    EXPECT_NE(written[1], source[1]);  // ricodificato, non copiato

    // Annulla a metà: niente file a metà.
    const fs::path cancelled = dir_ / "annullato.mp4";
    EXPECT_THROW(exportClips(plan, utf8FromPath(cancelled), AudioEdit{}, [](double fraction) { return fraction < 0.5; },
                             MuteMethod::Silence, &format, ReencodeOptions{QualityPreset::High, true}),
                 ExportCancelled);
    EXPECT_FALSE(fs::exists(cancelled));
}

TEST_F(Reencode, AnotherResolutionInTheFormatIsReencoded)
{
    // Il formato del progetto può venire da un file diverso dal primo pezzo: allora anche il primo si ricodifica.
    const ProjectFormat format = formatOf(readMediaSignature(file("b_720p30.mp4")));
    const fs::path out = dir_ / "formato_720p.mkv";
    const ExportResult result = exportClips({{file("a_1080p60.mp4"), {{0.0, 1.0}}}, {file("b_720p30.mp4"), {{0.0, 1.0}}}},
                                            utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format);
    EXPECT_EQ(result.reencodedSegments, 1);
    const Decoded decoded = decodeAll(out);
    EXPECT_EQ(decoded.errors, 0);
    EXPECT_EQ(decoded.width, 1280);
    EXPECT_EQ(decoded.height, 720);
    EXPECT_EQ(decoded.centre.size(), 60u);  // 30 al secondo
}

// --- Scelte dell'utente: encoder, preset, qualità (CQ) e audio

TEST_F(Reencode, EncoderChoicesAreUsedAndAnUnavailableEncoderIsRefused)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("a_1080p60.mp4")));
    const std::vector<ClipRanges> plan{{file("a_1080p60.mp4"), {{0.0, 1.0}}}};
    const auto software = findVideoEncoder(VideoCodec::H264, EncoderBackend::Software);
    if (!software)
        GTEST_SKIP() << "Nessun encoder software H.264 in questa build di FFmpeg.";

    // CQ basso = file più grande, con lo stesso encoder (x264) e preset veloce.
    const auto sizeAt = [&](int cq) {
        ReencodeOptions options{QualityPreset::High, true};  // "ricodifica tutto"
        options.backend = EncoderBackend::Software;
        options.preset = "ultrafast";
        options.cq = cq;
        const fs::path out = dir_ / ("cq" + std::to_string(cq) + ".mkv");
        const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
        EXPECT_EQ(result.reencodedSegments, 1);
        EXPECT_EQ(decodeAll(out).errors, 0);
        return fs::file_size(out);
    };
    EXPECT_GT(sizeAt(14), sizeAt(40));

    // Un encoder che su questo PC non c'è: l'esportazione non si può fare, e il riepilogo lo dice.
    for (const EncoderBackend backend : {EncoderBackend::Qsv, EncoderBackend::Amf, EncoderBackend::Nvenc}) {
        if (findVideoEncoder(VideoCodec::H264, backend))
            continue;
        ReencodeOptions options{QualityPreset::High, true};
        options.backend = backend;
        const ExportPreview preview = previewExport(plan, AudioEdit{}, &format, options);
        EXPECT_FALSE(preview.problem.empty());
        EXPECT_THROW(exportClips(plan, utf8FromPath(dir_ / "nessun_encoder.mkv"), AudioEdit{}, {}, MuteMethod::Silence, &format, options),
                     ClipsNotJoinable);
        EXPECT_FALSE(fs::exists(dir_ / "nessun_encoder.mkv"));
        break;
    }
}

TEST_F(Reencode, WithoutAudioTheFileHasNoAudioStream)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("a_1080p60.mp4")));
    ASSERT_TRUE(findStreamOfKind(probeFile(file("a_1080p60.mp4")), StreamKind::Audio)) << "il file di prova ha l'audio";
    for (const bool reencodeAll : {false, true}) {
        SCOPED_TRACE(reencodeAll ? "ricodificato" : "copiato");
        ReencodeOptions options{QualityPreset::High, reencodeAll};
        options.includeAudio = false;
        const fs::path out = dir_ / (reencodeAll ? "muto_ricodificato.mp4" : "muto_copiato.mp4");
        const std::vector<ClipRanges> plan{{file("a_1080p60.mp4"), {{0.0, 1.0}}}, {file("b_720p30.mp4"), {{0.0, 1.0}}}};
        const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
        EXPECT_GE(result.durationSeconds, 1.9);
        const MediaInfo info = probeFile(utf8FromPath(out));
        EXPECT_TRUE(findStreamOfKind(info, StreamKind::Video));
        EXPECT_FALSE(findStreamOfKind(info, StreamKind::Audio));
        EXPECT_EQ(decodeAll(out).errors, 0);
    }
}


}  // namespace
