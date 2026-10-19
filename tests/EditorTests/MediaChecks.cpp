#include "MediaChecks.h"

#include "EditorCore/AudioDecoder.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <map>

namespace editor::tests {

namespace {

constexpr double kFrame = 1.0 / 25.0;

}  // namespace

double lumaAt(const core::VideoFrame& frame, int x, int y)
{
    const std::uint8_t* p = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride + x * 4;
    return 16.0 + (p[0] + p[1] + p[2]) / 3.0 * 219.0 / 255.0;
}

void expectVideoFrames(const std::string& file, const std::vector<int>& frames, double timeTolerance)
{
    core::VideoDecoder video(file, {.allowHardware = false});
    auto frame = video.seek(0.0);
    std::size_t count = 0;
    while (frame) {
        if (count < frames.size()) {
            const int x = frame->width / 2;
            const int y = frame->height / 2;
            EXPECT_NEAR(frame->time, count * kFrame, timeTolerance) << "fotogramma " << count;
            EXPECT_NEAR(lumaAt(*frame, x, y), sampleLuma(x, y, frames[count]), 2.5)
                << "il fotogramma " << count << " dovrebbe essere il " << frames[count] << " dei file di prova";
        }
        ++count;
        frame = video.nextFrame();
    }
    EXPECT_EQ(count, frames.size());
}

void expectMonotonicPackets(const std::string& file)
{
    AVFormatContext* fmt = nullptr;
    ASSERT_GE(avformat_open_input(&fmt, file.c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(fmt, nullptr), 0);
    AVPacket* pkt = av_packet_alloc();
    std::map<int, std::int64_t> lastDts;
    bool firstVideo = true;
    bool hasVideo = false;
    while (av_read_frame(fmt, pkt) >= 0) {
        const AVStream* st = fmt->streams[pkt->stream_index];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            hasVideo = true;
            if (firstVideo) {
                EXPECT_NE(pkt->flags & AV_PKT_FLAG_KEY, 0) << "il primo pacchetto video deve essere un fotogramma chiave";
                firstVideo = false;
            }
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
    EXPECT_TRUE(hasVideo);
}

std::vector<std::int16_t> leftChannel(const std::string& file)
{
    core::AudioDecoder audio(file);
    std::vector<std::int16_t> left;
    while (auto chunk = audio.next()) {
        for (std::size_t i = 0; i < chunk->samples.size(); i += static_cast<std::size_t>(audio.channels()))
            left.push_back(chunk->samples[i]);
    }
    return left;
}

AudioTimeline audioTimeline(const std::string& file)
{
    core::AudioDecoder audio(file);
    AudioTimeline timeline;
    double previousEnd = -1.0;
    while (auto chunk = audio.next()) {
        const double length = chunk->samples.size() / static_cast<double>(audio.channels()) / audio.sampleRate();
        if (chunk->time < previousEnd - 0.001)
            timeline.overlaps = true;
        timeline.starts.push_back(chunk->time);
        previousEnd = chunk->time + length;
        timeline.totalSeconds += length;
    }
    return timeline;
}

}  // namespace editor::tests
