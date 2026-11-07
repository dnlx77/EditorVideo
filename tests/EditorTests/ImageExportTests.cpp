#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/StillImage.h"
#include "EditorCore/Transform.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

// Immagini nell'esportazione (step I4). File di prova come in TransformExportTests (H.264, 4 s, 30 fps, luminanza 100,
// audio AAC) e immagini BMP a tinta unica scritte a mano (così non serve nessun encoder di immagini).
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


// BMP a 24 bit w x h di un solo colore (grigio `value`), con la riga in alto a sinistra.
bool writeGrayBmp(const fs::path& path, int w, int h, std::uint8_t value)
{
    const int rowBytes = (w * 3 + 3) / 4 * 4;
    const std::uint32_t imageSize = static_cast<std::uint32_t>(rowBytes) * h;
    std::vector<std::uint8_t> file(54 + imageSize, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            file[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<std::uint32_t>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(w));
    put32(22, static_cast<std::uint32_t>(h));
    file[26] = 1;
    file[28] = 24;
    put32(34, imageSize);
    for (std::size_t i = 54; i < file.size(); ++i)
        file[i] = value;
    std::FILE* f = std::fopen(utf8FromPath(path).c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    return ok;
}

// Il volume (valore assoluto massimo dei campioni) dell'audio del file nel tratto [from, to) secondi; -1 se non ha audio.
int audioPeak(const fs::path& path, double from, double to)
{
    try {
        AudioDecoder decoder(utf8FromPath(path));
        long long position = 0;
        int peak = 0;
        const double rate = decoder.sampleRate();
        const int channels = decoder.channels();
        while (const auto chunk = decoder.next()) {
            for (std::size_t i = 0; i < chunk->samples.size(); ++i) {
                const double t = static_cast<double>(position + static_cast<long long>(i) / channels) / rate;
                if (t >= from && t < to)
                    peak = std::max(peak, std::abs(static_cast<int>(chunk->samples[i])));
            }
            position += static_cast<long long>(chunk->samples.size()) / channels;
        }
        return peak;
    } catch (const std::exception&) {
        return -1;
    }
}

class ImageExport : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoImmaginiEsportazione";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeClip(dir_ / "grigio.mp4") && writeGrayBmp(dir_ / "chiara.bmp", 64, 36, 220) && writeGrayBmp(dir_ / "verticale.bmp", 36, 64, 220);
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove delle immagini nell'esportazione si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    static inline fs::path dir_;
    static inline bool ok_ = false;
};

}  // namespace

TEST_F(ImageExport, ImageOnLayerIsComposedOverTheMainVideo)
{
    ClipRanges clip{file("grigio.mp4"), {{0.0, 4.0}}};
    clip.transforms = {Transform{}};
    ReencodeOptions options;
    const int layer = options.texts.addLayer();
    LayerPiece piece;
    piece.clipId = 7;
    piece.start = kStillCenter;
    piece.end = kStillCenter + 2.0;
    piece.position = 1.0;
    piece.transform.scaleX = piece.transform.scaleY = 0.5;
    ASSERT_NE(options.texts.addPiece(layer, piece), 0);
    options.layerFiles[7] = file("chiara.bmp");

    const fs::path out = dir_ / "immagineSuLivello.mp4";
    const ExportResult result = exportClips({clip}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, nullptr, options);
    EXPECT_EQ(result.textSegments, 1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const auto& p = d.pixels[static_cast<std::size_t>(i)];
        EXPECT_NEAR(p[1], 100, 8);  // l'angolo è sempre V1
        if (i < 28 || i > 92)
            EXPECT_NEAR(p[0], 100, 8);
        else if (i > 32 && i < 88)
            EXPECT_GT(p[0], 170);  // dentro: l'immagine chiara al centro
    }
}

TEST_F(ImageExport, ImageOnV1BetweenVideoPieces)
{
    // V1: 1 s di video, 2 s di immagine chiara, 1 s di video. Il formato del progetto è quello del video.
    const MediaSignature signature = readMediaSignature(file("grigio.mp4"));
    const ProjectFormat format = formatOf(signature);
    ClipRanges first{file("grigio.mp4"), {{0.0, 1.0}}};
    first.transforms = {Transform{}};
    ClipRanges image{file("chiara.bmp"), {{kStillCenter, kStillCenter + 2.0}}};
    image.transforms = {Transform{}};
    ClipRanges last{file("grigio.mp4"), {{1.0, 2.0}}};
    last.transforms = {Transform{}};
    const std::vector<ClipRanges> plan{first, image, last};

    const ExportPreview preview = previewExport(plan, AudioEdit{}, &format, {});
    EXPECT_TRUE(preview.problem.empty()) << preview.problem;

    const fs::path out = dir_ / "immagineInV1.mp4";
    const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, {});
    EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, 4.0, 0.1);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(4 * kFps));
    for (std::size_t i = 0; i < d.pixels.size(); ++i) {
        SCOPED_TRACE("fotogramma " + std::to_string(i));
        const auto& p = d.pixels[i];
        if (i < 28 || i > 92) {
            EXPECT_NEAR(p[0], 100, 8);
        } else if (i > 32 && i < 88) {
            EXPECT_GT(p[0], 170);  // l'immagine riempie il fotogramma (stesse proporzioni)
            EXPECT_GT(p[1], 170);
        }
    }
    EXPECT_GE(result.reencodedSegments, 1);
    // L'audio c'è dappertutto: suono sui pezzi di video, silenzio sotto l'immagine.
    EXPECT_GT(audioPeak(out, 0.1, 0.9), 1000);
    EXPECT_EQ(audioPeak(out, 1.2, 2.8), 0);
    EXPECT_GT(audioPeak(out, 3.1, 3.9), 1000);
}

TEST_F(ImageExport, ImageOnlyProjectUsesTheDefaultFormat)
{
    // Solo un'immagine (4 s), nel formato predefinito (H.264 1920x1080, 30 fps, AAC).
    const ProjectFormat format = defaultProjectFormat();
    ClipRanges image{file("chiara.bmp"), {{kStillCenter, kStillCenter + 2.0}}};
    image.transforms = {Transform{}};
    const fs::path out = dir_ / "soloImmagine.mp4";
    const ExportResult result = exportClips({image}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, {});
    EXPECT_NEAR(result.durationSeconds, 2.0, 0.1);
    const MediaInfo info = probeFile(utf8FromPath(out));
    bool video = false;
    for (const StreamInfo& s : info.streams) {
        if (s.kind == StreamKind::Video) {
            video = true;
            EXPECT_EQ(s.width, 1920);
            EXPECT_EQ(s.height, 1080);
        }
    }
    EXPECT_TRUE(video);
    EXPECT_NEAR(info.durationSeconds, 2.0, 0.15);
    EXPECT_EQ(audioPeak(out, 0.0, 2.0), 0);  // c'è una traccia audio, tutta silenzio
}

TEST_F(ImageExport, DissolveBetweenAVideoPieceAndAnImagePiece)
{
    Project project;
    const int video = project.add(file("grigio.mp4"));
    const int image = project.add(file("chiara.bmp"));
    project.setAnalysis(video, {0.0, 1.0, 2.0, 3.0}, 4.0);
    project.setAnalysis(image, {}, kStillDuration);
    project.setFormat(formatOf(readMediaSignature(file("grigio.mp4"))));
    const int first = project.sequence().insert(-1, video, 0.0, 2.0);
    ASSERT_NE(project.sequence().insert(-1, image, kStillCenter, kStillCenter + 2.0), 0);
    // L'immagine ha margini enormi: la transizione può essere lunga quanto si vuole.
    EXPECT_GE(project.transitionLimits(first).max(), 1.0);
    ASSERT_NE(project.addTransition(first, "dissolvenza", 1.0), 0);
    ReencodeOptions options;
    options.transitions = exportTransitionsOf(project);
    ASSERT_EQ(options.transitions.size(), 1u);
    const auto plan = project.exportPlan(GapExport::Black);
    const fs::path out = dir_ / "dissolvenzaImmagine.mp4";
    const ExportResult result =
        exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &project.format(), options);
    // La parte nel file video conta come transizione, quella nell'immagine (un file sempre ricodificato) come ricodifica.
    EXPECT_GE(result.transitionSegments + result.reencodedSegments, 2);
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(4 * kFps));
    EXPECT_NEAR(d.pixels[20][0], 100, 8);   // prima: il video
    EXPECT_GT(d.pixels[100][0], 170);       // dopo: l'immagine
    EXPECT_GT(d.pixels[60][0], 120);        // a metà: la fusione, fra i due
    EXPECT_LT(d.pixels[60][0], 190);
}

TEST_F(ImageExport, ImageWithOtherProportionsIsFittedWithBlackBars)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("grigio.mp4")));  // 640x360
    ClipRanges image{file("verticale.bmp"), {{kStillCenter, kStillCenter + 1.0}}};
    image.transforms = {Transform{}};
    const fs::path out = dir_ / "verticale.mp4";
    exportClips({image}, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, {});
    const Sampled d = sample(out);
    EXPECT_EQ(d.errors, 0);
    ASSERT_EQ(d.pixels.size(), static_cast<std::size_t>(kFps));
    EXPECT_GT(d.pixels[10][0], 170);  // al centro l'immagine
    EXPECT_LT(d.pixels[10][1], 40);   // l'angolo è una barra nera: l'immagine è intera, non tagliata
    EXPECT_LT(d.pixels[10][2], 40);   // e così il quarto destro (l'immagine verticale è stretta)
}
