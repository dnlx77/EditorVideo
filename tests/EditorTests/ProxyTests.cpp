#include "EditorCore/Encoder.h"
#include "EditorCore/MediaError.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Proxy.h"
#include "EditorCore/VideoDecoder.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::SampleDirTest;
using editor::tests::writeSampleFile;

namespace {

class ProxyTest : public SampleDirTest<ProxyTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoProxyTests";

    static void prepareSamples()
    {
        // Audio dal tempo 0, video da 0,2 s: l'inizio del file (quello dell'audio) non è quello del video.
        writeSampleFile(dir_ / "con_audio.mkv", {.video = true, .audio = true, .frames = 50, .gopSize = 50, .width = 320, .height = 240});
        delayVideo(dir_ / "con_audio.mkv", dir_ / "video_dopo.mkv", 0.2);
        // Fotogrammi chiave lontani (ogni 100 fotogrammi = 4 s), come i video che il proxy deve rendere leggeri.
        writeSampleFile(dir_ / "lontani.mkv", {.video = true, .audio = false, .frames = 150, .gopSize = 100, .width = 320, .height = 240});
        // MP4 con audio AAC: l'inizio del file può non coincidere con quello del video.
        writeSampleFile(dir_ / "telefono.mp4",
                        {.video = true, .audio = true, .frames = 60, .gopSize = 50, .width = 320, .height = 240, .aacAudio = true});
    }

    // Copia `from` in `to` spostando avanti di `seconds` i tempi del solo video.
    static void delayVideo(const fs::path& from, const fs::path& to, double seconds)
    {
        AVFormatContext* in = nullptr;
        ASSERT_GE(avformat_open_input(&in, utf8FromPath(from).c_str(), nullptr, nullptr), 0);
        ASSERT_GE(avformat_find_stream_info(in, nullptr), 0);
        AVFormatContext* out = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&out, nullptr, nullptr, utf8FromPath(to).c_str()), 0);
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            AVStream* st = avformat_new_stream(out, nullptr);
            avcodec_parameters_copy(st->codecpar, in->streams[i]->codecpar);
            st->codecpar->codec_tag = 0;
            st->time_base = in->streams[i]->time_base;
        }
        ASSERT_GE(avio_open(&out->pb, utf8FromPath(to).c_str(), AVIO_FLAG_WRITE), 0);
        ASSERT_GE(avformat_write_header(out, nullptr), 0);
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(in, packet) >= 0) {
            const AVStream* st = in->streams[packet->stream_index];
            av_packet_rescale_ts(packet, st->time_base, out->streams[packet->stream_index]->time_base);
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                const auto shift = av_rescale_q(std::llround(seconds * 1000), {1, 1000}, out->streams[packet->stream_index]->time_base);
                if (packet->pts != AV_NOPTS_VALUE)
                    packet->pts += shift;
                if (packet->dts != AV_NOPTS_VALUE)
                    packet->dts += shift;
            }
            av_interleaved_write_frame(out, packet);
        }
        av_write_trailer(out);
        av_packet_free(&packet);
        avio_closep(&out->pb);
        avformat_free_context(out);
        avformat_close_input(&in);
    }

protected:
    void SetUp() override
    {
        SampleDirTest::SetUp();
        if (IsSkipped())
            return;
        if (!preferredVideoEncoder(VideoCodec::H264))
            GTEST_SKIP() << "Nessun encoder H.264 funzionante su questo PC: prova saltata.";
    }

    // Tempi di tutti i fotogrammi del video.
    static std::vector<double> frameTimes(const std::string& file)
    {
        VideoDecoder decoder(file, {.allowHardware = false, .readAhead = false});
        std::vector<double> times;
        for (auto frame = decoder.seek(0.0); frame; frame = decoder.nextFrame())
            times.push_back(frame->time);
        return times;
    }

    // Indici dei pacchetti chiave del flusso video, in ordine.
    static std::vector<int> keyPackets(const std::string& file)
    {
        AVFormatContext* fmt = nullptr;
        EXPECT_GE(avformat_open_input(&fmt, file.c_str(), nullptr, nullptr), 0);
        std::vector<int> keys;
        if (fmt == nullptr)
            return keys;
        avformat_find_stream_info(fmt, nullptr);
        AVPacket* packet = av_packet_alloc();
        int index = 0;
        while (av_read_frame(fmt, packet) >= 0) {
            if (fmt->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (packet->flags & AV_PKT_FLAG_KEY)
                    keys.push_back(index);
                ++index;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&fmt);
        return keys;
    }
};

TEST_F(ProxyTest, FileNameDependsOnOriginalAndDivisor)
{
    const std::string original = path("lontani.mkv");
    const std::string half = proxyFileName(original, 2);
    EXPECT_FALSE(half.empty());
    EXPECT_EQ(half, proxyFileName(original, 2));  // stabile
    EXPECT_NE(half, proxyFileName(original, 4));
    EXPECT_EQ(half.rfind("lontani_", 0), 0u) << half;
    EXPECT_NE(half.find("_1-2.mp4"), std::string::npos) << half;
    EXPECT_TRUE(proxyFileName(path("non_esiste.mkv"), 2).empty());

    // L'originale cambia (qui solo la data): nome diverso, il vecchio proxy non vale più.
    const fs::path copy = dir_ / "copia.mkv";
    fs::copy_file(dir_ / "lontani.mkv", copy, fs::copy_options::overwrite_existing);
    const std::string before = proxyFileName(utf8FromPath(copy), 2);
    fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::seconds(10));
    EXPECT_NE(before, proxyFileName(utf8FromPath(copy), 2));
}

TEST_F(ProxyTest, SizeIsAFractionOfTheOriginal)
{
    EXPECT_EQ(proxySize(3840, 2160, 2), std::make_pair(1920, 1080));
    EXPECT_EQ(proxySize(3840, 2160, 4), std::make_pair(960, 540));
    EXPECT_EQ(proxySize(3840, 2160, 8), std::make_pair(480, 270));
    EXPECT_EQ(proxySize(1920, 1080, 8), std::make_pair(240, 136));  // pari
}

TEST_F(ProxyTest, ProxyHasTheSameFramesAtTheSameTimesAndFrequentKeyframes)
{
    for (const char* name : {"lontani.mkv", "telefono.mp4", "video_dopo.mkv"}) {
        SCOPED_TRACE(name);
        const std::string original = path(name);
        const std::string proxy = utf8FromPath(dir_ / "proxy" / proxyFileName(original, 2));
        std::vector<double> reported;
        buildProxy(original, proxy, 2, [&](double p) { reported.push_back(p); });
        ASSERT_TRUE(fs::exists(pathFromUtf8(proxy)));
        EXPECT_FALSE(fs::exists(pathFromUtf8(proxy + ".part")));
        ASSERT_FALSE(reported.empty());
        EXPECT_DOUBLE_EQ(reported.back(), 1.0);

        // Stessi fotogrammi, stessi tempi.
        const std::vector<double> times = frameTimes(original);
        const std::vector<double> proxyTimes = frameTimes(proxy);
        ASSERT_EQ(proxyTimes.size(), times.size());
        for (std::size_t i = 0; i < times.size(); ++i)
            EXPECT_NEAR(proxyTimes[i], times[i], 1e-6) << "fotogramma " << i;

        // Un fotogramma chiave almeno ogni kProxyKeyframeInterval.
        const std::vector<int> keys = keyPackets(proxy);
        ASSERT_FALSE(keys.empty());
        EXPECT_EQ(keys.front(), 0);
        for (std::size_t i = 1; i < keys.size(); ++i)
            EXPECT_LE(keys[i] - keys[i - 1], kProxyKeyframeInterval);
        EXPECT_LE(static_cast<int>(times.size()) - 1 - keys.back(), kProxyKeyframeInterval);

        // Metà delle misure, e il contenuto è lo stesso dell'originale (luminanza al centro, con la tolleranza della
        // codifica).
        VideoDecoder decoder(proxy, {.allowHardware = false});
        VideoDecoder reference(original, {.allowHardware = false});
        EXPECT_EQ(decoder.outputWidth(), 160);
        EXPECT_EQ(decoder.outputHeight(), 120);
        const auto centerLuma = [](const VideoFrame& frame) {
            const std::uint8_t* p = frame.pixels.data() + static_cast<std::size_t>(frame.height / 2) * frame.stride + frame.width / 2 * 4;
            return (p[0] + p[1] + p[2]) / 3.0;
        };
        for (const std::size_t index : {std::size_t{0}, times.size() / 2, times.size() - 1}) {
            const auto frame = decoder.seek(times[index]);
            const auto expected = reference.seek(times[index]);
            ASSERT_NE(frame, nullptr);
            ASSERT_NE(expected, nullptr);
            EXPECT_NEAR(frame->time, times[index], 1e-6);
            EXPECT_NEAR(centerLuma(*frame), centerLuma(*expected), 10.0) << "fotogramma " << index;
        }
    }
}

TEST_F(ProxyTest, CancelLeavesNoFiles)
{
    const std::string proxy = utf8FromPath(dir_ / "annullato" / "proxy.mp4");
    std::atomic<bool> cancel{true};
    EXPECT_THROW(buildProxy(path("lontani.mkv"), proxy, 4, {}, &cancel), MediaError);
    EXPECT_FALSE(fs::exists(pathFromUtf8(proxy)));
    EXPECT_FALSE(fs::exists(pathFromUtf8(proxy + ".part")));
}

}  // namespace
