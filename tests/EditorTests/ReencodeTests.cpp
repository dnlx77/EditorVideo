#include "EditorCore/Compatibility.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/SegmentExportInternal.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::decodeVideoFrames;
using editor::tests::lumaAt;
using editor::tests::writeEncodedClip;

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
    return writeEncodedClip(path, {.codec = spec.codec, .width = spec.width, .height = spec.height, .fps = spec.fps,
                                   .frames = spec.frames, .gop = spec.gop, .luma = lumaOf(spec, 0), .lumaStep = 1,
                                   .audioRate = spec.audioRate, .audioAmplitude = 12000});
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
    d.errors = decodeVideoFrames(path, [&](const AVFrame& f) {
        d.width = f.width;
        d.height = f.height;
        d.centre.push_back(lumaAt(f, f.width / 2, f.height / 2));
        d.edge.push_back(lumaAt(f, 8, f.height / 2));
    }, true);
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


// --- Il video del file creato diverso da quello dei file: codec, misura, fotogrammi al secondo

TEST_F(Reencode, ChosenResolutionFrameRateAndCodecChangeEveryPiece)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("a_1080p60.mp4")));
    const std::vector<ClipRanges> plan{{file("a_1080p60.mp4"), {{0.0, 1.0}}}, {file("a_1080p60.mp4"), {{1.0, 2.0}}}};

    // Misura e fotogrammi al secondo: tutto si ricodifica e il file ha il video voluto.
    {
        ReencodeOptions options;
        options.video.width = 1280;
        options.video.height = 720;
        options.video.frameRate = 30.0;
        const ExportPreview preview = previewExport(plan, AudioEdit{}, &format, options);
        EXPECT_TRUE(preview.problem.empty()) << preview.problem;
        EXPECT_EQ(preview.copiedSegments, 0);
        EXPECT_EQ(preview.reencodedSegments, 2);
        const fs::path out = dir_ / "scelta_720p30.mp4";
        exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
        const Decoded decoded = decodeAll(out);
        EXPECT_EQ(decoded.errors, 0);
        EXPECT_EQ(decoded.width, 1280);
        EXPECT_EQ(decoded.height, 720);
        EXPECT_EQ(decoded.centre.size(), 60u);  // 2 s a 30 al secondo
    }

    // Le stesse misure dei file: non cambia niente, i pezzi si copiano.
    {
        ReencodeOptions options;
        options.video.width = 1920;
        options.video.height = 1080;
        options.video.frameRate = 60.0;
        const ExportPreview preview = previewExport(plan, AudioEdit{}, &format, options);
        EXPECT_EQ(preview.reencodedSegments, 0);
    }

    // Un altro codec (HEVC), anche in un altro contenitore.
    if (preferredVideoEncoder(VideoCodec::Hevc)) {
        ReencodeOptions options;
        options.video.codec = VideoCodec::Hevc;
        const fs::path out = dir_ / "scelta_hevc.mkv";
        exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
        const MediaInfo info = probeFile(utf8FromPath(out));
        const auto video = std::find_if(info.streams.begin(), info.streams.end(),
                                        [](const StreamInfo& stream) { return stream.kind == StreamKind::Video; });
        ASSERT_NE(video, info.streams.end());
        EXPECT_EQ(video->codec, "hevc");
        EXPECT_EQ(decodeAll(out).errors, 0);
    }

    // HEVC a metà risoluzione, in MKV e in MP4: il video c'è davvero (fotogrammi decodificati, misura giusta).
    if (preferredVideoEncoder(VideoCodec::Hevc)) {
        for (const char* extension : {"mkv", "mp4"}) {
            SCOPED_TRACE(extension);
            ReencodeOptions options;
            options.video.codec = VideoCodec::Hevc;
            options.video.width = 960;
            options.video.height = 540;
            const fs::path out = dir_ / (std::string("scelta_hevc_540.") + extension);
            exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
            const Decoded decoded = decodeAll(out);
            EXPECT_EQ(decoded.errors, 0);
            EXPECT_EQ(decoded.width, 960);
            EXPECT_EQ(decoded.height, 540);
            EXPECT_EQ(decoded.centre.size(), 120u);  // 2 s a 60 al secondo
        }
    }

    // AV1 (se c'è un encoder): MKV e MP4; nella build non c'è un decoder AV1 software, quindi si rilegge solo il file.
    if (preferredVideoEncoder(VideoCodec::Av1)) {
        for (const char* extension : {"mkv", "mp4"}) {
            SCOPED_TRACE(extension);
            ReencodeOptions options;
            options.video.codec = VideoCodec::Av1;
            options.video.width = 640;
            options.video.height = 360;
            const fs::path out = dir_ / (std::string("scelta_av1.") + extension);
            const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
            EXPECT_GE(result.durationSeconds, 1.9);
            const MediaInfo info = probeFile(utf8FromPath(out));
            const auto video = std::find_if(info.streams.begin(), info.streams.end(),
                                            [](const StreamInfo& stream) { return stream.kind == StreamKind::Video; });
            ASSERT_NE(video, info.streams.end());
            EXPECT_EQ(video->codec, "av1");
            EXPECT_EQ(video->width, 640);
            EXPECT_EQ(video->height, 360);
            EXPECT_GE(fs::file_size(out), 1000u);
        }
    }
}


// --- Decoder dei video originali: CPU o GPU (senza GPU si ricade sulla CPU, il risultato è lo stesso)

TEST_F(Reencode, GpuDecoderGivesTheSameVideoAsTheCpuOne)
{
    const ProjectFormat format = formatOf(readMediaSignature(file("a_1080p60.mp4")));
    const std::vector<ClipRanges> plan{{file("a_1080p60.mp4"), {{0.0, 1.0}}}};
    Decoded results[2];
    int index = 0;
    for (const DecoderMode mode : {DecoderMode::Software, DecoderMode::Hardware}) {
        ReencodeOptions options{QualityPreset::High, true};  // "ricodifica tutto"
        options.decoder = mode;
        const fs::path out = dir_ / (index == 0 ? "decoder_cpu.mp4" : "decoder_gpu.mp4");
        const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{}, {}, MuteMethod::Silence, &format, options);
        EXPECT_EQ(result.reencodedSegments, 1);
        results[index] = decodeAll(out);
        EXPECT_EQ(results[index].errors, 0);
        ++index;
    }
    ASSERT_EQ(results[0].centre.size(), results[1].centre.size());
    EXPECT_EQ(results[0].width, results[1].width);
    EXPECT_EQ(results[0].height, results[1].height);
    for (std::size_t i = 0; i < results[0].centre.size(); ++i)
        EXPECT_NEAR(results[0].centre[i], results[1].centre[i], 4) << "fotogramma " << i;
}


}  // namespace

// --- La codifica in un thread suo ---

namespace {
detail::FramePtr testFrame(int pts)
{
    detail::FramePtr frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_GRAY8;
    frame->width = 16;
    frame->height = 16;
    av_frame_get_buffer(frame.get(), 0);
    frame->pts = pts;
    return frame;
}
}  // namespace

TEST(AsyncFrameEncoder, EncodesEveryFrameInOrderWhileTheCallerKeepsGoing)
{
    std::vector<std::int64_t> seen;
    std::vector<bool> keys;
    {
        segment::AsyncFrameEncoder encoder([&](const AVFrame* frame, bool key) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            seen.push_back(frame->pts);
            keys.push_back(key);
        }, 2);
        for (int i = 0; i < 50; ++i) {
            const detail::FramePtr frame = testFrame(i);
            encoder.push(frame.get(), i == 0);
        }
        encoder.drain();
        EXPECT_EQ(seen.size(), 50u);
    }
    ASSERT_EQ(seen.size(), 50u);
    for (int i = 0; i < 50; ++i)
        EXPECT_EQ(seen[static_cast<std::size_t>(i)], i);
    EXPECT_TRUE(keys.front());
    EXPECT_FALSE(keys.back());
}

TEST(AsyncFrameEncoder, AnErrorInTheThreadComesBackToTheCaller)
{
    segment::AsyncFrameEncoder encoder([&](const AVFrame* frame, bool) {
        if (frame->pts == 3)
            throw MediaError("codifica fallita");
    }, 2);
    bool thrown = false;
    try {
        for (int i = 0; i < 20; ++i) {
            const detail::FramePtr frame = testFrame(i);
            encoder.push(frame.get(), false);
        }
        encoder.drain();
    } catch (const MediaError& error) {
        thrown = std::string(error.what()).find("codifica fallita") != std::string::npos;
    }
    EXPECT_TRUE(thrown);
}

TEST(AsyncFrameEncoder, DestroyingItWithFramesQueuedDoesNotHang)
{
    std::atomic<int> encoded{0};
    {
        segment::AsyncFrameEncoder encoder([&](const AVFrame*, bool) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ++encoded;
        }, 3);
        for (int i = 0; i < 3; ++i) {
            const detail::FramePtr frame = testFrame(i);
            encoder.push(frame.get(), false);
        }
    }
    EXPECT_LE(encoded.load(), 3);
}

// --- Lettura e decodifica in un thread suo ---

TEST(DecodeThread, DeliversEveryFrameInOrderAndThenEnds)
{
    segment::DecodeThread thread(2, [](segment::DecodeThread::Channel& channel) {
        for (int i = 0; i < 40; ++i)
            channel.push(testFrame(i));
    });
    int expected = 0;
    while (const detail::FramePtr frame = thread.pop())
        EXPECT_EQ(frame->pts, expected++);
    EXPECT_EQ(expected, 40);
    thread.join();
}

TEST(DecodeThread, AProducerErrorComesBackToTheConsumer)
{
    segment::DecodeThread thread(2, [](segment::DecodeThread::Channel& channel) {
        channel.push(testFrame(0));
        throw MediaError("lettura fallita");
    });
    bool thrown = false;
    try {
        while (thread.pop()) {
        }
    } catch (const MediaError& error) {
        thrown = std::string(error.what()).find("lettura fallita") != std::string::npos;
    }
    EXPECT_TRUE(thrown);
}

TEST(DecodeThread, StoppingTheFramesLetsTheProducerFinishItsOtherWork)
{
    std::atomic<int> pushedAfterStop{0};
    std::atomic<bool> finishedOtherWork{false};
    {
        segment::DecodeThread thread(1, [&](segment::DecodeThread::Channel& channel) {
            for (int i = 0; i < 1000; ++i)
                if (!channel.push(testFrame(i)))
                    ++pushedAfterStop;  // scartato: chi consuma non li vuole più
            finishedOtherWork = true;
        });
        ASSERT_NE(thread.pop(), nullptr);
        thread.stopFrames();
        thread.join();
    }
    EXPECT_TRUE(finishedOtherWork.load());
    EXPECT_GT(pushedAfterStop.load(), 0);
}

TEST(DecodeThread, DestroyingItWhileTheProducerIsBlockedDoesNotHang)
{
    segment::DecodeThread thread(1, [](segment::DecodeThread::Channel& channel) {
        for (int i = 0; i < 100; ++i)
            channel.push(testFrame(i));
    });
    SUCCEED();
}
