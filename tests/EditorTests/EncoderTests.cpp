#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/VideoDecoder.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFrames = 125;  // 5 s a 25 fps
constexpr int kForcedKeyframes[] = {37, 80};

// Luminanza del fotogramma `index` (range TV): ogni fotogramma 1 livello più chiaro, con una sfumatura leggera.
int lumaOf(int x, int y, int index)
{
    return 40 + index + (x + y) / 32;
}

// Scrive in un file i pacchetti degli encoder (un flusso video e, se c'è, uno audio).
class Writer
{
public:
    Writer(const fs::path& file, const VideoEncoder* video, const AudioEncoder* audio)
    {
        const std::string path = utf8FromPath(file);
        if (avformat_alloc_output_context2(&fmt_, nullptr, nullptr, path.c_str()) < 0)
            throw std::runtime_error("contenitore non creato");
        if (video != nullptr) {
            videoStream_ = avformat_new_stream(fmt_, nullptr);
            video->copyParameters(videoStream_->codecpar);
            const Rational tb = video->timeBase();
            videoStream_->time_base = {tb.num, tb.den};
            videoTb_ = {tb.num, tb.den};
        }
        if (audio != nullptr) {
            audioStream_ = avformat_new_stream(fmt_, nullptr);
            audio->copyParameters(audioStream_->codecpar);
            const Rational tb = audio->timeBase();
            audioStream_->time_base = {tb.num, tb.den};
            audioTb_ = {tb.num, tb.den};
        }
        if (avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(fmt_, nullptr) < 0)
            throw std::runtime_error("file non scritto");
    }

    ~Writer()
    {
        if (fmt_ != nullptr) {
            avio_closep(&fmt_->pb);
            avformat_free_context(fmt_);
        }
    }

    PacketSink videoSink()
    {
        return [this](AVPacket* p) { write(p, videoStream_, videoTb_); };
    }
    PacketSink audioSink()
    {
        return [this](AVPacket* p) { write(p, audioStream_, audioTb_); };
    }

    void close()
    {
        av_write_trailer(fmt_);
        avio_closep(&fmt_->pb);
    }

    int videoPackets = 0;
    int videoKeyPackets = 0;

private:
    void write(AVPacket* packet, AVStream* stream, AVRational tb)
    {
        if (stream == videoStream_) {
            ++videoPackets;
            videoKeyPackets += (packet->flags & AV_PKT_FLAG_KEY) != 0 ? 1 : 0;
        }
        AVPacket* copy = av_packet_clone(packet);
        av_packet_rescale_ts(copy, tb, stream->time_base);
        copy->stream_index = stream->index;
        av_interleaved_write_frame(fmt_, copy);
        av_packet_free(&copy);
    }

    AVFormatContext* fmt_ = nullptr;
    AVStream* videoStream_ = nullptr;
    AVStream* audioStream_ = nullptr;
    AVRational videoTb_{1, 25};
    AVRational audioTb_{1, 48000};
};

using FramePtr = std::unique_ptr<AVFrame, decltype([](AVFrame* f) { av_frame_free(&f); })>;

// Fotogramma di prova in YUV 4:2:0 (grigio: colore 128).
FramePtr testFrame(int index)
{
    FramePtr frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = kWidth;
    frame->height = kHeight;
    av_frame_get_buffer(frame.get(), 0);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x)
            frame->data[0][y * frame->linesize[0] + x] = static_cast<std::uint8_t>(lumaOf(x, y, index));
    }
    for (int plane = 1; plane <= 2; ++plane) {
        for (int y = 0; y < kHeight / 2; ++y)
            std::fill(frame->data[plane] + y * frame->linesize[plane], frame->data[plane] + y * frame->linesize[plane] + kWidth / 2, 128);
    }
    frame->pts = index;
    return frame;
}

// Audio di prova: `count` campioni stereo PCM 16 bit a 48 kHz, dal campione `first`.
FramePtr testAudio(long long first, int count)
{
    FramePtr frame(av_frame_alloc());
    frame->format = AV_SAMPLE_FMT_S16;
    frame->sample_rate = 48000;
    frame->nb_samples = count;
    av_channel_layout_default(&frame->ch_layout, 2);
    av_frame_get_buffer(frame.get(), 0);
    auto* samples = reinterpret_cast<std::int16_t*>(frame->data[0]);
    for (int i = 0; i < count; ++i) {
        // Un La (440 Hz) a metà volume: l'AAC lo riproduce fedelmente.
        const auto v = static_cast<std::int16_t>(16000 * std::sin(2 * 3.14159265358979 * 440 * (first + i) / 48000.0));
        samples[2 * i] = v;
        samples[2 * i + 1] = v;
    }
    return frame;
}

fs::path testDir()
{
    const fs::path dir = fs::temp_directory_path() / "EditorVideoEncoderTests";
    fs::create_directories(dir);
    return dir;
}

TEST(Encoders, ListOfWorkingEncoders)
{
    const auto& encoders = availableVideoEncoders();
    std::printf("Encoder video funzionanti su questo PC: %d\n", static_cast<int>(encoders.size()));
    for (const VideoEncoderInfo& info : encoders) {
        std::printf("  %s\n", describe(info).c_str());
        EXPECT_FALSE(info.ffmpegName.empty());
    }
    // La seconda volta il risultato è già pronto (e uguale).
    EXPECT_EQ(&availableVideoEncoders(), &encoders);
    for (VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1}) {
        const auto preferred = preferredVideoEncoder(codec);
        const auto first = std::find_if(encoders.begin(), encoders.end(), [&](const VideoEncoderInfo& i) { return i.codec == codec; });
        EXPECT_EQ(preferred.has_value(), first != encoders.end());
        if (preferred)
            EXPECT_EQ(preferred->ffmpegName, first->ffmpegName);
    }
}

// Ogni encoder funzionante: 5 s di video (con due fotogrammi chiave chiesti) e audio AAC in un MKV, poi riletti.
TEST(Encoders, EveryWorkingEncoderMakesAPlayableFile)
{
    const auto& encoders = availableVideoEncoders();
    if (encoders.empty())
        GTEST_SKIP() << "Nessun encoder video funzionante su questo PC (es. un server senza scheda video né Media Foundation): "
                        "prova saltata. NVENC si prova sul PC con la scheda NVIDIA.";
    for (const VideoEncoderInfo& info : encoders) {
        SCOPED_TRACE(describe(info));
        for (QualityPreset quality : {QualityPreset::High, QualityPreset::Medium}) {
            const fs::path file = testDir() / (info.ffmpegName + (quality == QualityPreset::High ? "_alta.mkv" : "_media.mkv"));
            VideoEncoderSettings settings;
            settings.codec = info.codec;
            settings.backend = info.backend;
            settings.width = kWidth;
            settings.height = kHeight;
            settings.quality = quality;
            settings.gopFrames = 50;  // fotogrammi chiave anche ogni 2 s
            int videoPackets = 0;
            int keyPackets = 0;
            {
                VideoEncoder video(settings);
                ASSERT_EQ(video.info().ffmpegName, info.ffmpegName);
                AudioEncoder audio({});
                Writer writer(file, &video, &audio);
                const PacketSink videoSink = writer.videoSink();
                const PacketSink audioSink = writer.audioSink();
                for (int i = 0; i < kFrames; ++i) {
                    const bool forced = std::find(std::begin(kForcedKeyframes), std::end(kForcedKeyframes), i) != std::end(kForcedKeyframes);
                    video.encode(testFrame(i).get(), forced, videoSink);
                    audio.encode(testAudio(i * 1920LL, 1920).get(), audioSink);  // 1920 campioni = 1/25 s
                }
                video.finish(videoSink);
                audio.finish(audioSink);
                writer.close();
                videoPackets = writer.videoPackets;
                keyPackets = writer.videoKeyPackets;
            }
            EXPECT_EQ(videoPackets, kFrames);

            const MediaInfo probed = probeFile(utf8FromPath(file));
            EXPECT_NEAR(probed.durationSeconds, 5.0, 0.1);
            ASSERT_GE(probed.streams.size(), 2u);
            // Fotogrammi chiave: all'inizio, dove chiesti e almeno ogni 2 s.
            const auto hasKeyframe = [&](double t) {
                return std::any_of(probed.keyframeTimes.begin(), probed.keyframeTimes.end(),
                                   [&](double k) { return std::abs(k - t) < 0.01; });
            };
            EXPECT_TRUE(hasKeyframe(0.0));
            for (int forced : kForcedKeyframes)
                EXPECT_TRUE(hasKeyframe(forced / 25.0)) << "fotogramma chiave chiesto al fotogramma " << forced;
            EXPECT_GE(keyPackets, 3);
            for (std::size_t k = 1; k < probed.keyframeTimes.size(); ++k)
                EXPECT_LE(probed.keyframeTimes[k] - probed.keyframeTimes[k - 1], 2.0 + 0.01);

            // Tutti i fotogrammi si decodificano, nell'ordine e con la luminosità giusta. L'AV1 questa build di FFmpeg lo
            // decodifica solo con la scheda video (non ha un decoder AV1 software).
            VideoDecoder decoder(utf8FromPath(file), {.allowHardware = info.codec == VideoCodec::Av1, .maxWidth = kWidth, .maxHeight = kHeight});
            auto frame = decoder.seek(0.0);
            int count = 0;
            double worst = 0.0;
            while (frame) {
                const int index = static_cast<int>(std::lround(frame->time * 25));
                for (const auto& [x, y] : {std::pair{100, 100}, std::pair{320, 180}, std::pair{600, 300}}) {
                    const double difference = std::abs(editor::tests::lumaAt(*frame, x, y) - lumaOf(x, y, index));
                    worst = std::max(worst, difference);
                }
                ++count;
                frame = decoder.nextFrame();
            }
            EXPECT_EQ(count, kFrames);
            EXPECT_LT(worst, quality == QualityPreset::High ? 6.0 : 10.0) << "luminosità diversa dall'originale";
            std::printf("  %s, qualità %s: %d fotogrammi, %d chiave, %.0f kB, differenza massima di luminosità %.1f\n",
                        describe(info).c_str(), quality == QualityPreset::High ? "alta" : "media", count,
                        static_cast<int>(probed.keyframeTimes.size()), fs::file_size(file) / 1024.0, worst);
        }
    }
}

TEST(Encoders, InBandParametersNeedNoExtradata)
{
    const auto h264 = preferredVideoEncoder(VideoCodec::H264);
    if (!h264)
        GTEST_SKIP() << "Nessun encoder H.264 funzionante su questo PC: prova saltata.";
    VideoEncoderSettings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.inBandParameters = true;
    VideoEncoder video(settings);
    // Ogni fotogramma chiave porta con sé SPS (tipo 7) e PPS (tipo 8), in formato Annex B.
    int keyframes = 0;
    int withParameters = 0;
    const PacketSink sink = [&](AVPacket* p) {
        if ((p->flags & AV_PKT_FLAG_KEY) == 0)
            return;
        ++keyframes;
        bool sps = false;
        bool pps = false;
        for (int i = 0; i + 3 < p->size; ++i) {
            if (p->data[i] == 0 && p->data[i + 1] == 0 && p->data[i + 2] == 1) {
                const int type = p->data[i + 3] & 0x1f;
                sps = sps || type == 7;
                pps = pps || type == 8;
            }
        }
        withParameters += sps && pps ? 1 : 0;
    };
    for (int i = 0; i < 60; ++i)
        video.encode(testFrame(i).get(), i == 0 || i == 30, sink);
    video.finish(sink);
    EXPECT_GE(keyframes, 2);
    EXPECT_EQ(withParameters, keyframes) << describe(video.info());
}

TEST(Encoders, AacFromAnyAudioFormat)
{
    const fs::path file = testDir() / "audio.m4a";
    {
        AudioEncoder audio({.sampleRate = 48000, .channels = 2});
        Writer writer(file, nullptr, &audio);
        const PacketSink sink = writer.audioSink();
        long long sent = 0;
        // Blocchi di misure diverse (non multipli di 1024): l'encoder li riunisce.
        for (int size : {100, 2000, 777, 48000, 1, 45122}) {
            audio.encode(testAudio(sent, size).get(), sink);
            sent += size;
        }
        audio.finish(sink);
        writer.close();
        ASSERT_EQ(sent, 96000);
    }
    const MediaInfo probed = probeFile(utf8FromPath(file));
    ASSERT_EQ(probed.streams.size(), 1u);
    EXPECT_EQ(probed.streams[0].codec, "aac");
    EXPECT_NEAR(probed.durationSeconds, 2.0, 0.05);
}

// --- Preset, qualità (CQ) e codec audio scelti dall'utente

// Quanti byte producono 40 fotogrammi di prova con queste impostazioni.
std::int64_t videoBytes(const VideoEncoderInfo& info, const std::string& preset, int cq)
{
    VideoEncoderSettings settings;
    settings.codec = info.codec;
    settings.backend = info.backend;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.preset = preset;
    settings.cq = cq;
    VideoEncoder video(settings);
    std::int64_t bytes = 0;
    const PacketSink sink = [&](AVPacket* packet) { bytes += packet->size; };
    for (int i = 0; i < 40; ++i)
        video.encode(testFrame(i).get(), i == 0, sink);
    video.finish(sink);
    return bytes;
}

TEST(Encoders, TuningDescribesEveryWorkingEncoder)
{
    for (const VideoEncoderInfo& info : availableVideoEncoders()) {
        const VideoEncoderTuning tuning = videoEncoderTuning(info);
        SCOPED_TRACE(describe(info));
        EXPECT_LT(tuning.cqMin, tuning.cqHigh);
        EXPECT_LT(tuning.cqHigh, tuning.cqMedium);
        EXPECT_LE(tuning.cqMedium, tuning.cqMax);
        if (tuning.presets.empty()) {
            EXPECT_TRUE(tuning.defaultPreset.empty());
        } else {
            const auto it = std::find_if(tuning.presets.begin(), tuning.presets.end(),
                                         [&](const EncoderPreset& p) { return p.value == tuning.defaultPreset; });
            EXPECT_NE(it, tuning.presets.end());
        }
    }
}

TEST(Encoders, LowerCqMakesBiggerFilesAndEveryPresetOpens)
{
    for (const VideoEncoderInfo& info : availableVideoEncoders()) {
        if (info.codec == VideoCodec::Av1)
            continue;  // la prova su AV1 la fa già l'elenco; qui bastano i codec dell'esportazione
        SCOPED_TRACE(describe(info));
        const VideoEncoderTuning tuning = videoEncoderTuning(info);
        const std::string preset = tuning.defaultPreset;
        const std::int64_t good = videoBytes(info, preset, tuning.cqMin + 8);
        const std::int64_t poor = videoBytes(info, preset, std::min(tuning.cqMax, tuning.cqMin + 40));
        EXPECT_GT(good, 0);
        EXPECT_GT(good, poor);
        // Ogni preset si apre e produce dati.
        for (const EncoderPreset& p : tuning.presets)
            EXPECT_GT(videoBytes(info, p.value, tuning.cqHigh), 0) << p.value;
    }
}

// La build di FFmpeg di questo progetto include x264, x265, MP3 e Opus (vedi vcpkg.json): se mancano, il pannello di
// esportazione non li potrebbe offrire.
TEST(Encoders, BuildHasSoftwareEncodersAndAllAudioCodecs)
{
    for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        const auto& encoders = availableVideoEncoders();
        EXPECT_TRUE(std::any_of(encoders.begin(), encoders.end(), [&](const VideoEncoderInfo& i) {
            return i.codec == codec && i.backend == EncoderBackend::Software;
        })) << codecName(codec);
    }
    const auto& audio = availableAudioCodecs();
    for (const AudioCodec codec : {AudioCodec::Aac, AudioCodec::Mp3, AudioCodec::Opus, AudioCodec::Ac3, AudioCodec::Flac})
        EXPECT_NE(std::find(audio.begin(), audio.end(), codec), audio.end()) << audioCodecName(codec);
}

TEST(Encoders, AudioCodecListStartsWithAac)
{
    const auto& codecs = availableAudioCodecs();
    ASSERT_FALSE(codecs.empty());
    EXPECT_EQ(codecs.front(), AudioCodec::Aac);
    for (const AudioCodec codec : codecs) {
        EXPECT_FALSE(audioCodecName(codec).empty());
        EXPECT_EQ(audioBitRatesKbps(codec).empty(), codec == AudioCodec::Flac);
        EXPECT_GT(defaultAudioBitRateKbps(codec), 0);
    }
}

TEST(Encoders, ContainersAcceptTheirUsualCodecs)
{
    EXPECT_TRUE(audioCodecFitsContainer(AudioCodec::Aac, "mp4"));
    EXPECT_TRUE(audioCodecFitsContainer(AudioCodec::Aac, ".MKV"));
    EXPECT_TRUE(videoCodecFitsContainer(VideoCodec::H264, "mp4"));
    EXPECT_TRUE(videoCodecFitsContainer(VideoCodec::Hevc, ".mkv"));
    // Estensione sconosciuta: nessun contenitore.
    EXPECT_FALSE(audioCodecFitsContainer(AudioCodec::Aac, "qualcosa"));
    EXPECT_FALSE(videoCodecFitsContainer(VideoCodec::H264, ""));
}

// Ogni codec audio disponibile: un secondo di audio in un file Matroska, poi riletto col codec giusto.
TEST(Encoders, EveryAudioCodecMakesAFile)
{
    const std::map<AudioCodec, std::string> names = {{AudioCodec::Aac, "aac"},   {AudioCodec::Mp3, "mp3"}, {AudioCodec::Opus, "opus"},
                                                    {AudioCodec::Ac3, "ac3"},   {AudioCodec::Flac, "flac"}};
    for (const AudioCodec codec : availableAudioCodecs()) {
        SCOPED_TRACE(audioCodecName(codec));
        ASSERT_TRUE(audioCodecFitsContainer(codec, "mka"));
        const fs::path file = testDir() / ("tutti-" + names.at(codec) + ".mka");
        {
            AudioEncoder audio({.sampleRate = 48000, .channels = 2, .bitRate = 0, .codec = codec});
            Writer writer(file, nullptr, &audio);
            const PacketSink sink = writer.audioSink();
            for (long long sent = 0; sent < 48000; sent += 1000)
                audio.encode(testAudio(sent, 1000).get(), sink);
            audio.finish(sink);
            writer.close();
        }
        const MediaInfo probed = probeFile(utf8FromPath(file));
        ASSERT_EQ(probed.streams.size(), 1u);
        EXPECT_EQ(probed.streams[0].codec, names.at(codec));
        EXPECT_NEAR(probed.durationSeconds, 1.0, 0.1);
    }
}

TEST(Encoders, AudioBitRateIsRespected)
{
    const auto sizeAt = [&](int bitRate) {
        const fs::path file = testDir() / ("bitrate-" + std::to_string(bitRate) + ".m4a");
        {
            AudioEncoder audio({.sampleRate = 48000, .channels = 2, .bitRate = bitRate});
            Writer writer(file, nullptr, &audio);
            const PacketSink sink = writer.audioSink();
            for (long long sent = 0; sent < 96000; sent += 1000)
                audio.encode(testAudio(sent, 1000).get(), sink);
            audio.finish(sink);
            writer.close();
        }
        return fs::file_size(file);
    };
    EXPECT_GT(sizeAt(256000), 2 * sizeAt(64000));
}

TEST(Encoders, ImpossibleRequestsAreRefusedWithAMessage)
{
    VideoEncoderSettings settings;
    settings.width = 0;
    EXPECT_THROW(VideoEncoder{settings}, MediaError);
}

}  // namespace
