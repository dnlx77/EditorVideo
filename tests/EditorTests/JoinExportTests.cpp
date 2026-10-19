#include "EditorCore/AudioDecoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/SegmentList.h"
#include "EditorCore/VideoDecoder.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <cmath>
#include <filesystem>
#include <limits>
#include <map>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::sampleAudio;
using editor::tests::sampleLuma;
using editor::tests::SampleSpec;
using editor::tests::toUtf8;
using editor::tests::writeSampleFile;

namespace {

constexpr double kFrame = 1.0 / 25.0;
constexpr int kRate = 48000;

// Sorgente da 4 s (100 fotogrammi), fotogrammi chiave a 0, 1, 2, 3 s. Tre tratti che iniziano su fotogrammi chiave
// e finiscono su confini di fotogramma: fotogrammi 0..14, 25..34, 75..82 => 33 fotogrammi, 1,32 s.
const std::vector<TimeRange> kRanges = {{0.0, 0.6}, {1.0, 1.4}, {3.0, 3.32}};

std::vector<int> expectedFrames(const std::vector<TimeRange>& ranges)
{
    std::vector<int> frames;
    for (const TimeRange& r : ranges) {
        for (int i = static_cast<int>(std::lround(r.start / kFrame)); i < static_cast<int>(std::lround(r.end / kFrame)); ++i)
            frames.push_back(i);
    }
    return frames;
}

double totalLength(const std::vector<TimeRange>& ranges)
{
    double total = 0.0;
    for (const TimeRange& r : ranges)
        total += r.length();
    return total;
}

class JoinExportTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoJoinTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!editor::tests::sampleEncodersAvailable())
            return;
        const SampleSpec base{.video = true, .audio = true, .frames = 100, .gopSize = 25};
        writeSampleFile(dir_ / "sorgente.mkv", base);
        SampleSpec mp4 = base;
        mp4.aacAudio = true;
        writeSampleFile(dir_ / "sorgente.mp4", mp4);
        writeSampleFile(dir_ / "sorgente.wav", {.video = false, .audio = true, .frames = 100});
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void SetUp() override
    {
        if (!editor::tests::sampleEncodersAvailable())
            GTEST_SKIP() << "FFmpeg senza gli encoder necessari: impossibile generare i file di prova";
    }

    static std::string path(const char* name) { return toUtf8(dir_ / name); }

    static double lumaAt(const VideoFrame& frame, int x, int y)
    {
        const std::uint8_t* p = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride + x * 4;
        return 16.0 + (p[0] + p[1] + p[2]) / 3.0 * 219.0 / 255.0;
    }

    // Il video del file creato deve essere esattamente la sequenza di fotogrammi attesa, a tempi continui.
    static void expectVideoFrames(const std::string& file, const std::vector<int>& frames)
    {
        VideoDecoder video(file, {.allowHardware = false});
        auto frame = video.seek(0.0);
        std::size_t count = 0;
        while (frame) {
            if (count < frames.size()) {
                const int x = frame->width / 2;
                const int y = frame->height / 2;
                EXPECT_NEAR(frame->time, count * kFrame, 0.002) << "fotogramma " << count;
                EXPECT_NEAR(lumaAt(*frame, x, y), sampleLuma(x, y, frames[count]), 2.5)
                    << "il fotogramma " << count << " dovrebbe essere il " << frames[count] << " dell'originale";
            }
            ++count;
            frame = video.nextFrame();
        }
        EXPECT_EQ(count, frames.size());
    }

    // Su ogni flusso i tempi di decodifica crescono sempre; il primo pacchetto video è un fotogramma chiave.
    static void expectMonotonicPackets(const std::string& file)
    {
        AVFormatContext* fmt = nullptr;
        ASSERT_GE(avformat_open_input(&fmt, file.c_str(), nullptr, nullptr), 0);
        ASSERT_GE(avformat_find_stream_info(fmt, nullptr), 0);
        AVPacket* pkt = av_packet_alloc();
        std::map<int, std::int64_t> lastDts;
        bool firstVideo = true;
        while (av_read_frame(fmt, pkt) >= 0) {
            const AVStream* st = fmt->streams[pkt->stream_index];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && firstVideo) {
                EXPECT_NE(pkt->flags & AV_PKT_FLAG_KEY, 0) << "il primo pacchetto video deve essere un fotogramma chiave";
                firstVideo = false;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                const auto it = lastDts.find(pkt->stream_index);
                if (it != lastDts.end())
                    EXPECT_GT(pkt->dts, it->second) << "flusso " << pkt->stream_index << ": DTS non crescente";
                lastDts[pkt->stream_index] = pkt->dts;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avformat_close_input(&fmt);
        EXPECT_FALSE(firstVideo);
    }

    static inline fs::path dir_;
};

TEST_F(JoinExportTest, ThreeRangesFromMatroska)
{
    const std::string exported = path("tre_tratti.mkv");
    const ExportResult result = exportSegments(path("sorgente.mkv"), exported, kRanges);
    EXPECT_EQ(result.segmentCount, 3);
    EXPECT_NEAR(result.durationSeconds, totalLength(kRanges), 2 * kFrame);

    const MediaInfo info = probeFile(exported);
    EXPECT_NE(info.formatName.find("matroska"), std::string::npos);
    ASSERT_EQ(info.streams.size(), 2u);
    EXPECT_EQ(info.streams[0].codec, "mpeg4");
    EXPECT_EQ(info.streams[1].codec, "pcm_s16le");
    EXPECT_NEAR(info.durationSeconds, totalLength(kRanges), 2 * kFrame);

    expectVideoFrames(exported, expectedFrames(kRanges));
    expectMonotonicPackets(exported);

    // Audio PCM: campione per campione, a ogni giunzione riparte esattamente dall'inizio del tratto successivo.
    AudioDecoder audio(exported);
    std::vector<std::int16_t> left;
    while (auto chunk = audio.next()) {
        for (std::size_t i = 0; i < chunk->samples.size(); i += 2)
            left.push_back(chunk->samples[i]);
    }
    EXPECT_EQ(static_cast<long long>(left.size()), std::llround(totalLength(kRanges) * kRate));
    long long joinAt = 0;
    for (const TimeRange& r : kRanges) {
        const auto first = std::llround(r.start * kRate);
        const auto count = std::llround(r.length() * kRate);
        ASSERT_LT(joinAt + count - 1, static_cast<long long>(left.size()));
        EXPECT_EQ(left[joinAt], sampleAudio(first)) << "inizio del tratto da " << r.start << " s";
        EXPECT_EQ(left[joinAt + count - 1], sampleAudio(first + count - 1)) << "fine del tratto fino a " << r.end << " s";
        joinAt += count;
    }
}

TEST_F(JoinExportTest, ThreeRangesFromMp4WithAac)
{
    const std::string exported = path("tre_tratti.mp4");
    exportSegments(path("sorgente.mp4"), exported, kRanges);

    const MediaInfo info = probeFile(exported);
    EXPECT_NE(info.formatName.find("mp4"), std::string::npos);
    ASSERT_EQ(info.streams.size(), 2u);
    EXPECT_EQ(info.streams[1].codec, "aac");
    EXPECT_NEAR(info.durationSeconds, totalLength(kRanges), 2 * kFrame);

    expectVideoFrames(exported, expectedFrames(kRanges));
    expectMonotonicPackets(exported);

    // AAC (blocchi da 1024 campioni, ~21 ms): a ogni giunzione l'audio riparte entro un blocco dall'inizio del
    // tratto nel file creato, e i blocchi non si sovrappongono mai.
    AudioDecoder audio(exported);
    std::vector<double> starts;
    double previousEnd = -1.0;
    double total = 0.0;
    while (auto chunk = audio.next()) {
        const double length = chunk->samples.size() / 2.0 / kRate;
        EXPECT_GE(chunk->time, previousEnd - 0.001) << "audio sovrapposto a " << chunk->time << " s";
        starts.push_back(chunk->time);
        previousEnd = chunk->time + length;
        total += length;
    }
    double join = 0.0;
    for (const TimeRange& r : kRanges) {
        const bool found = std::any_of(starts.begin(), starts.end(), [&](double t) { return t >= join - 0.001 && t <= join + 0.025; });
        EXPECT_TRUE(found) << "l'audio non riparte alla giunzione di " << join << " s";
        join += r.length();
    }
    EXPECT_LE(total, totalLength(kRanges) + 0.001);
    EXPECT_GE(total, totalLength(kRanges) - 3 * 2 * 0.0214);  // al più un blocco perso all'inizio e alla fine di ogni tratto
}

TEST_F(JoinExportTest, CuttingAwayMiddlePart)
{
    // "Togli la pubblicità": dall'elenco vuoto si toglie 1,5 .. 3,0 s. Resta 0 .. 1,5 e 3,0 .. 4,0.
    const std::string source = path("sorgente.mkv");
    const MediaInfo info = probeFile(source);
    SegmentList list;
    list.reset(info.keyframeTimes, info.durationSeconds);
    ASSERT_EQ(list.remove(1.5, 3.0), SegmentList::RemoveResult::Removed);
    ASSERT_EQ(list.ranges().size(), 2u);

    const std::string exported = path("senza_pubblicita.mkv");
    exportSegments(source, exported, list.ranges());
    EXPECT_NEAR(probeFile(exported).durationSeconds, 2.5, 2 * kFrame);
    expectVideoFrames(exported, expectedFrames(list.ranges()));  // 0..37 poi 75..99
    expectMonotonicPackets(exported);
}

TEST_F(JoinExportTest, SingleRangeStillWorks)
{
    const std::string exported = path("uno.mkv");
    const std::vector<TimeRange> one = {{1.0, 2.6}};
    const ExportResult result = exportSegments(path("sorgente.mkv"), exported, one);
    EXPECT_EQ(result.segmentCount, 1);
    expectVideoFrames(exported, expectedFrames(one));
    expectMonotonicPackets(exported);
}

TEST_F(JoinExportTest, PcmAudioOnlyIsCutAtTheExactSample)
{
    // Solo audio PCM (WAV): i bordi non coincidono con i pacchetti, ma il taglio deve essere al campione esatto,
    // senza silenzi alle giunzioni.
    const std::vector<TimeRange> ranges = {{0.1234, 0.9}, {2.0137, 2.5}};
    const std::string exported = path("solo_audio.wav");
    exportSegments(path("sorgente.wav"), exported, ranges);

    AudioDecoder audio(exported);
    std::vector<std::int16_t> left;
    while (auto chunk = audio.next()) {
        for (std::size_t i = 0; i < chunk->samples.size(); i += 2)
            left.push_back(chunk->samples[i]);
    }
    long long expected = 0;
    for (const TimeRange& r : ranges)
        expected += std::llround(r.end * kRate) - static_cast<long long>(std::ceil(r.start * kRate - 1e-6));
    EXPECT_NEAR(static_cast<double>(left.size()), static_cast<double>(expected), 2.0);

    long long at = 0;
    for (const TimeRange& r : ranges) {
        const auto first = static_cast<long long>(std::ceil(r.start * kRate - 1e-6));
        const auto count = std::llround(r.end * kRate) - first;
        ASSERT_LE(at + count, static_cast<long long>(left.size()) + 2);
        EXPECT_EQ(left[at], sampleAudio(first)) << "inizio del tratto da " << r.start << " s";
        EXPECT_EQ(left[at + count - 2], sampleAudio(first + count - 2)) << "fine del tratto fino a " << r.end << " s";
        at += count;
    }
}

TEST_F(JoinExportTest, EmptyListIsRejected)
{
    EXPECT_THROW(exportSegments(path("sorgente.mkv"), path("vuoto.mkv"), {}), MediaError);
    EXPECT_FALSE(fs::exists(dir_ / "vuoto.mkv"));
}

}  // namespace
