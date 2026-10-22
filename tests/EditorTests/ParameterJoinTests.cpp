#include "EditorCore/Compatibility.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/SegmentExport.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

// Formato del progetto e classificazione dei pezzi, con caratteristiche scritte a mano.

StreamSignature videoSignature()
{
    StreamSignature s;
    s.kind = StreamKind::Video;
    s.codec = "h264";
    s.profile = "High";
    s.width = 1920;
    s.height = 1080;
    s.pixelFormat = "yuv420p";
    s.frameRate = 60.0;
    return s;
}

StreamSignature audioSignature()
{
    StreamSignature s;
    s.kind = StreamKind::Audio;
    s.codec = "aac";
    s.profile = "LC";
    s.sampleRate = 48000;
    s.channels = 2;
    s.channelLayout = "stereo";
    return s;
}

MediaSignature file(const std::string& name, std::vector<StreamSignature> streams)
{
    return {"C:\\video\\" + name, name, std::move(streams)};
}

TEST(ProjectFormat, ClassifiesEveryPieceAgainstTheFormat)
{
    const ProjectFormat format = formatOf(file("primo.mp4", {videoSignature(), audioSignature()}));
    EXPECT_EQ(format.sourceName, "primo.mp4");
    ASSERT_NE(format.video(), nullptr);
    ASSERT_NE(format.audio(), nullptr);

    // Uguale: si copia.
    EXPECT_EQ(classifyClip(file("uguale.mp4", {videoSignature(), audioSignature()}), format).fit, ClipFit::Copy);

    // Solo un altro profilo H.264: si copia con i suoi parametri.
    StreamSignature main = videoSignature();
    main.profile = "Main";
    const ClipFitResult profile = classifyClip(file("main.mp4", {main, audioSignature()}), format);
    EXPECT_EQ(profile.fit, ClipFit::CopyWithParameters);
    ASSERT_EQ(profile.reasons.size(), 1u);
    EXPECT_NE(profile.reasons[0].find("Main"), std::string::npos);

    // Risoluzione o fotogrammi al secondo diversi: da ricodificare, con il motivo.
    StreamSignature small = videoSignature();
    small.width = 1280;
    small.height = 720;
    const ClipFitResult resized = classifyClip(file("piccolo.mp4", {small, audioSignature()}), format);
    EXPECT_EQ(resized.fit, ClipFit::Reencode);
    ASSERT_FALSE(resized.reasons.empty());
    EXPECT_NE(resized.reasons[0].find("1280x720"), std::string::npos);
    StreamSignature slow = videoSignature();
    slow.frameRate = 30.0;
    EXPECT_EQ(classifyClip(file("lento.mp4", {slow, audioSignature()}), format).fit, ClipFit::Reencode);

    // Audio diverso: da ricodificare, ma non se l'audio non conta (traccia sostitutiva).
    StreamSignature mono = audioSignature();
    mono.channels = 1;
    mono.channelLayout = "mono";
    EXPECT_EQ(classifyClip(file("mono.mp4", {videoSignature(), mono}), format).fit, ClipFit::Reencode);
    EXPECT_EQ(classifyClip(file("mono.mp4", {videoSignature(), mono}), format, false).fit, ClipFit::Copy);

    // Senza audio, o un altro codec: da ricodificare.
    EXPECT_EQ(classifyClip(file("muto.mp4", {videoSignature()}), format).fit, ClipFit::Reencode);
    StreamSignature hevc = videoSignature();
    hevc.codec = "hevc";
    hevc.profile = "Main";
    EXPECT_EQ(classifyClip(file("hevc.mp4", {hevc, audioSignature()}), format).fit, ClipFit::Reencode);

    // Per un codec che non sa ripetere i parametri nel flusso, impostazioni diverse sono da ricodificare.
    StreamSignature mpeg4 = videoSignature();
    mpeg4.codec = "mpeg4";
    mpeg4.profile.clear();
    mpeg4.extradata = {1, 2, 3};
    const ProjectFormat mpeg4Format = formatOf(file("a.avi", {mpeg4}));
    mpeg4.extradata = {1, 2, 4};
    EXPECT_EQ(classifyClip(file("b.avi", {mpeg4}), mpeg4Format).fit, ClipFit::Reencode);
}

TEST(ProjectFormat, SavedWithTheProjectAndUndone)
{
    Project project;
    const int a = project.add("C:\\video\\a.mp4");
    project.setAnalysis(a, {0.0, 1.0}, 2.0);
    EXPECT_TRUE(project.format().empty());
    const Project before = project;
    project.sequence().insert(-1, a, 0.0, 2.0);
    StreamSignature v = videoSignature();
    v.extradata = {1, 100, 0, 40, 0xff, 0xe1};
    project.setFormat(formatOf(file("a.mp4", {v, audioSignature()})));

    const fs::path path = fs::temp_directory_path() / "EditorVideoFormato.evproj";
    saveProjectFile(utf8FromPath(path), project, {});
    const LoadedProject loaded = loadProjectFile(utf8FromPath(path));
    fs::remove(path);
    const ProjectFormat& f = loaded.project.format();
    ASSERT_EQ(f.streams.size(), 2u);
    EXPECT_EQ(f.sourceName, "a.mp4");
    EXPECT_EQ(f.streams[0].codec, "h264");
    EXPECT_EQ(f.streams[0].profile, "High");
    EXPECT_EQ(f.streams[0].width, 1920);
    EXPECT_EQ(f.streams[0].height, 1080);
    EXPECT_DOUBLE_EQ(f.streams[0].frameRate, 60.0);
    EXPECT_EQ(f.streams[0].pixelFormat, "yuv420p");
    EXPECT_EQ(f.streams[0].extradata, v.extradata);
    EXPECT_EQ(f.streams[1].sampleRate, 48000);
    EXPECT_EQ(f.streams[1].channelLayout, "stereo");

    // Annulla: il progetto torna senza formato.
    project.restoreEdits(before);
    EXPECT_TRUE(project.format().empty());
}

// ---------------------------------------------------------------------------------------------------------------
// Unione di file H.264 con parametri diversi (servono gli encoder della scheda video: senza, le prove si saltano).

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFrames = 50;  // 2 s a 25 fps, un fotogramma chiave al secondo

int lumaOf(int x, int y, int index)
{
    return 30 + index + (x + y) / 16;
}

// Scrive un file H.264 di prova (fotogrammi `first` .. `first` + kFrames - 1); false se non si può.
bool writeH264(const fs::path& path, int first, int referenceFrames, EncoderBackend backend)
{
    VideoEncoderSettings s;
    s.width = kWidth;
    s.height = kHeight;
    s.gopFrames = 25;
    s.backend = backend;
    s.referenceFrames = referenceFrames;
    try {
        VideoEncoder encoder(s);
        AVFormatContext* fmt = nullptr;
        const std::string name = utf8FromPath(path);
        avformat_alloc_output_context2(&fmt, nullptr, nullptr, name.c_str());
        AVStream* st = avformat_new_stream(fmt, nullptr);
        encoder.copyParameters(st->codecpar);
        st->time_base = {1, 25};
        avio_open(&fmt->pb, name.c_str(), AVIO_FLAG_WRITE);
        if (avformat_write_header(fmt, nullptr) < 0)
            return false;
        const PacketSink sink = [&](AVPacket* p) {
            AVPacket* copy = av_packet_clone(p);
            av_packet_rescale_ts(copy, {1, 25}, st->time_base);
            copy->stream_index = 0;
            av_interleaved_write_frame(fmt, copy);
            av_packet_free(&copy);
        };
        for (int i = 0; i < kFrames; ++i) {
            AVFrame* frame = av_frame_alloc();
            frame->format = AV_PIX_FMT_YUV420P;
            frame->width = kWidth;
            frame->height = kHeight;
            av_frame_get_buffer(frame, 0);
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x)
                    frame->data[0][y * frame->linesize[0] + x] = static_cast<std::uint8_t>(lumaOf(x, y, first + i));
            }
            for (int p = 1; p <= 2; ++p) {
                for (int y = 0; y < kHeight / 2; ++y)
                    std::fill_n(frame->data[p] + y * frame->linesize[p], kWidth / 2, 128);
            }
            frame->pts = i;
            encoder.encode(frame, false, sink);
            av_frame_free(&frame);
        }
        encoder.finish(sink);
        av_write_trailer(fmt);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return true;
    } catch (const MediaError&) {
        return false;
    }
}

struct Packet
{
    std::vector<std::uint8_t> data;
    bool key = false;
};

std::vector<Packet> videoPackets(const fs::path& path, unsigned* codecTag = nullptr)
{
    AVFormatContext* fmt = nullptr;
    std::vector<Packet> out;
    if (avformat_open_input(&fmt, utf8FromPath(path).c_str(), nullptr, nullptr) < 0)
        return out;
    avformat_find_stream_info(fmt, nullptr);
    const int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (codecTag != nullptr)
        *codecTag = fmt->streams[video]->codecpar->codec_tag;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video)
            out.push_back({{pkt->data, pkt->data + pkt->size}, (pkt->flags & AV_PKT_FLAG_KEY) != 0});
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return out;
}

// Decodifica tutto il video (decoder software di FFmpeg, che segnala gli errori) e restituisce la luminanza di un
// pixel di ogni fotogramma; `errors` = errori del decoder.
std::vector<int> decodeLuma(const fs::path& path, int& errors)
{
    std::vector<int> luma;
    errors = 0;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8FromPath(path).c_str(), nullptr, nullptr) < 0) {
        errors = 1;
        return luma;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[video]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[video]->codecpar);
    ctx->err_recognition = AV_EF_EXPLODE | AV_EF_CRCCHECK | AV_EF_BITSTREAM;
    avcodec_open2(ctx, codec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            luma.push_back(frame->data[0][40 * frame->linesize[0] + 40]);
            errors += (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) ? 1 : 0;
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video) {
            if (avcodec_send_packet(ctx, pkt) < 0)
                ++errors;
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
    return luma;
}

class ParameterJoin : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoParametri";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        // Due file H.264 con 3 e 4 fotogrammi di riferimento (parametri diversi, tutto il resto uguale).
        for (EncoderBackend backend : {EncoderBackend::Nvenc, EncoderBackend::MediaFoundation}) {
            const auto encoder = std::find_if(availableVideoEncoders().begin(), availableVideoEncoders().end(),
                                              [&](const VideoEncoderInfo& i) { return i.codec == VideoCodec::H264 && i.backend == backend; });
            if (encoder == availableVideoEncoders().end())
                continue;
            if (writeH264(dir_ / "tre.mp4", 0, 3, backend) && writeH264(dir_ / "quattro.mp4", 100, 4, backend)
                && readMediaSignature(utf8FromPath(dir_ / "tre.mp4")).streams[0].extradata
                       != readMediaSignature(utf8FromPath(dir_ / "quattro.mp4")).streams[0].extradata) {
                encoderName_ = describe(*encoder);
                return;
            }
        }
    }

    void SetUp() override
    {
        if (encoderName_.empty())
            GTEST_SKIP() << "Nessun encoder H.264 che produca due file con parametri diversi su questo PC: prova saltata.";
    }

    static inline fs::path dir_;
    static inline std::string encoderName_;
};

TEST_F(ParameterJoin, DifferentParametersAreCopiedWithParametersInTheStream)
{
    const std::string three = utf8FromPath(dir_ / "tre.mp4");
    const std::string four = utf8FromPath(dir_ / "quattro.mp4");
    const ClipFitResult fit = classifyClip(readMediaSignature(four), formatOf(readMediaSignature(three)));
    EXPECT_EQ(fit.fit, ClipFit::CopyWithParameters) << encoderName_;
    for (const std::string& reason : fit.reasons)
        std::printf("  differenza (%s): %s\n", encoderName_.c_str(), reason.c_str());

    const std::vector<Packet> sourceThree = videoPackets(dir_ / "tre.mp4");
    const std::vector<Packet> sourceFour = videoPackets(dir_ / "quattro.mp4");
    ASSERT_EQ(sourceThree.size(), static_cast<std::size_t>(kFrames));
    ASSERT_EQ(sourceFour.size(), static_cast<std::size_t>(kFrames));

    // Tre pezzi: 3 rif. intero, 4 rif. intero, 3 rif. dal secondo 1 (fotogramma chiave).
    const std::vector<ClipRanges> plan{{three, {{0.0, 2.0}}}, {four, {{0.0, 2.0}}}, {three, {{1.0, 2.0}}}};
    std::vector<int> expected;
    for (int i = 0; i < kFrames; ++i)
        expected.push_back(lumaOf(40, 40, i));
    for (int i = 0; i < kFrames; ++i)
        expected.push_back(lumaOf(40, 40, 100 + i));
    for (int i = 25; i < kFrames; ++i)
        expected.push_back(lumaOf(40, 40, i));
    std::vector<const Packet*> copied;  // i pacchetti originali, nell'ordine del file creato
    for (const Packet& p : sourceThree)
        copied.push_back(&p);
    for (const Packet& p : sourceFour)
        copied.push_back(&p);
    for (int i = 25; i < kFrames; ++i)
        copied.push_back(&sourceThree[static_cast<std::size_t>(i)]);

    for (const char* extension : {".mp4", ".mkv"}) {
        SCOPED_TRACE(extension);
        const fs::path out = dir_ / (std::string("unione") + extension);
        const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{});
        EXPECT_TRUE(result.parametersInStream);

        // Tutti i fotogrammi si decodificano senza errori, nell'ordine giusto (anche dopo le giunzioni).
        int errors = 0;
        const std::vector<int> luma = decodeLuma(out, errors);
        EXPECT_EQ(errors, 0);
        ASSERT_EQ(luma.size(), expected.size());
        int wrong = 0;
        for (std::size_t i = 0; i < luma.size(); ++i)
            wrong += std::abs(luma[i] - expected[i]) > 4 ? 1 : 0;
        EXPECT_EQ(wrong, 0) << "fotogrammi con la luminosità sbagliata";

        // Pacchetti copiati: identici agli originali; i fotogrammi chiave hanno in più, davanti, i parametri.
        unsigned tag = 0;
        const std::vector<Packet> written = videoPackets(out, &tag);
        ASSERT_EQ(written.size(), copied.size());
        int different = 0;
        for (std::size_t i = 0; i < written.size(); ++i) {
            const auto& a = written[i].data;
            const auto& b = copied[i]->data;
            const bool same = written[i].key ? a.size() > b.size() && std::equal(b.begin(), b.end(), a.end() - static_cast<std::ptrdiff_t>(b.size()))
                                             : a == b;
            different += same ? 0 : 1;
        }
        EXPECT_EQ(different, 0);
        if (std::string(extension) == ".mp4")
            EXPECT_EQ(tag, MKTAG('a', 'v', 'c', '3'));
    }
}

TEST_F(ParameterJoin, SameParametersStayAsBefore)
{
    const std::string three = utf8FromPath(dir_ / "tre.mp4");
    const fs::path out = dir_ / "stessi.mp4";
    const ExportResult result = exportClips({{three, {{0.0, 1.0}}}, {three, {{1.0, 2.0}}}}, utf8FromPath(out), AudioEdit{});
    EXPECT_FALSE(result.parametersInStream);
    unsigned tag = 0;
    const std::vector<Packet> written = videoPackets(out, &tag);
    EXPECT_EQ(tag, MKTAG('a', 'v', 'c', '1'));
    const std::vector<Packet> source = videoPackets(dir_ / "tre.mp4");
    ASSERT_EQ(written.size(), source.size());
    for (std::size_t i = 0; i < written.size(); ++i)
        EXPECT_EQ(written[i].data, source[i].data) << "pacchetto " << i;
}

TEST_F(ParameterJoin, TheProjectFormatDecidesWhatIsRefused)
{
    // Con il formato dell'altro file si copia lo stesso (anche nell'altro verso cambiano altri parametri delle immagini).
    const std::string three = utf8FromPath(dir_ / "tre.mp4");
    const std::string four = utf8FromPath(dir_ / "quattro.mp4");
    EXPECT_NE(classifyClip(readMediaSignature(three), formatOf(readMediaSignature(four))).fit, ClipFit::Reencode);
    // Un formato con un'altra risoluzione: il pezzo si ricodifica in quella risoluzione (step R3).
    ProjectFormat other = formatOf(readMediaSignature(three));
    other.streams[0].width = 640;
    other.streams[0].height = 480;
    const ExportResult result = exportClips({{three, {{0.0, 1.0}}}}, utf8FromPath(dir_ / "ricodificato.mp4"), AudioEdit{}, {},
                                            MuteMethod::Silence, &other);
    EXPECT_EQ(result.reencodedSegments, 1);
}

}  // namespace
