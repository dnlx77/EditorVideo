#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <stdexcept>
#include <string>

namespace editor::tests {

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;
constexpr int kFps = 25;
constexpr int kSampleRate = 48000;
constexpr int kSamplesPerFrame = kSampleRate / kFps;

void check(int ret, const char* what)
{
    if (ret < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, buffer, sizeof(buffer));
        throw std::runtime_error(std::string(what) + ": " + buffer);
    }
}

struct Track
{
    AVCodecContext* enc = nullptr;
    AVStream* stream = nullptr;
};

class Writer
{
public:
    explicit Writer(const std::string& path)
    {
        check(avformat_alloc_output_context2(&fmt_, nullptr, nullptr, path.c_str()), "avformat_alloc_output_context2");
        path_ = path;
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
    }

    ~Writer()
    {
        avcodec_free_context(&video_.enc);
        avcodec_free_context(&audio_.enc);
        if (fmt_ != nullptr && (fmt_->oformat->flags & AVFMT_NOFILE) == 0)
            avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        av_packet_free(&packet_);
        av_frame_free(&frame_);
    }

    void addVideo(int gopSize)
    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        video_.enc = avcodec_alloc_context3(codec);
        video_.enc->width = kWidth;
        video_.enc->height = kHeight;
        video_.enc->pix_fmt = AV_PIX_FMT_YUV420P;
        video_.enc->time_base = {1, kFps};
        video_.enc->framerate = {kFps, 1};
        video_.enc->gop_size = gopSize;
        video_.enc->max_b_frames = 0;
        // Niente fotogrammi chiave extra sui cambi di scena: vogliamo esattamente uno ogni gopSize.
        av_opt_set_int(video_.enc, "sc_threshold", 1'000'000'000, AV_OPT_SEARCH_CHILDREN);
        open(video_, codec);
    }

    void addAudio()
    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        audio_.enc = avcodec_alloc_context3(codec);
        audio_.enc->sample_fmt = AV_SAMPLE_FMT_S16;
        audio_.enc->sample_rate = kSampleRate;
        const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(av_channel_layout_copy(&audio_.enc->ch_layout, &stereo), "av_channel_layout_copy");
        audio_.enc->time_base = {1, kSampleRate};
        open(audio_, codec);
    }

    void write(int frames)
    {
        if ((fmt_->oformat->flags & AVFMT_NOFILE) == 0)
            check(avio_open(&fmt_->pb, path_.c_str(), AVIO_FLAG_WRITE), "avio_open");
        check(avformat_write_header(fmt_, nullptr), "avformat_write_header");

        for (int i = 0; i < frames; ++i) {
            if (video_.enc != nullptr)
                encode(video_, makeVideoFrame(i));
            if (audio_.enc != nullptr)
                encode(audio_, makeAudioFrame(i));
        }
        if (video_.enc != nullptr)
            encode(video_, nullptr);
        if (audio_.enc != nullptr)
            encode(audio_, nullptr);

        check(av_write_trailer(fmt_), "av_write_trailer");
    }

private:
    void open(Track& track, const AVCodec* codec)
    {
        if ((fmt_->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            track.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(track.enc, codec, nullptr), "avcodec_open2");
        track.stream = avformat_new_stream(fmt_, nullptr);
        check(avcodec_parameters_from_context(track.stream->codecpar, track.enc), "avcodec_parameters_from_context");
        track.stream->time_base = track.enc->time_base;
    }

    AVFrame* makeVideoFrame(int index)
    {
        av_frame_unref(frame_);
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width = kWidth;
        frame_->height = kHeight;
        check(av_frame_get_buffer(frame_, 0), "av_frame_get_buffer");
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                frame_->data[0][y * frame_->linesize[0] + x] = static_cast<uint8_t>(x + y + index * 3);
        for (int plane = 1; plane < 3; ++plane)
            for (int y = 0; y < kHeight / 2; ++y)
                for (int x = 0; x < kWidth / 2; ++x)
                    frame_->data[plane][y * frame_->linesize[plane] + x] = 128;
        frame_->pts = index;
        return frame_;
    }

    AVFrame* makeAudioFrame(int index)
    {
        av_frame_unref(frame_);
        frame_->format = AV_SAMPLE_FMT_S16;
        frame_->sample_rate = kSampleRate;
        frame_->nb_samples = kSamplesPerFrame;
        check(av_channel_layout_copy(&frame_->ch_layout, &audio_.enc->ch_layout), "av_channel_layout_copy");
        check(av_frame_get_buffer(frame_, 0), "av_frame_get_buffer");
        auto* samples = reinterpret_cast<int16_t*>(frame_->data[0]);
        for (int s = 0; s < kSamplesPerFrame * 2; ++s)
            samples[s] = static_cast<int16_t>((s % 100) * 100);
        frame_->pts = static_cast<int64_t>(index) * kSamplesPerFrame;
        return frame_;
    }

    void encode(Track& track, AVFrame* frame)
    {
        check(avcodec_send_frame(track.enc, frame), "avcodec_send_frame");
        for (;;) {
            const int ret = avcodec_receive_packet(track.enc, packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return;
            check(ret, "avcodec_receive_packet");
            av_packet_rescale_ts(packet_, track.enc->time_base, track.stream->time_base);
            packet_->stream_index = track.stream->index;
            check(av_interleaved_write_frame(fmt_, packet_), "av_interleaved_write_frame");
        }
    }

    AVFormatContext* fmt_ = nullptr;
    std::string path_;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    Track video_;
    Track audio_;
};

}  // namespace

std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool sampleEncodersAvailable()
{
    return avcodec_find_encoder(AV_CODEC_ID_MPEG4) != nullptr && avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE) != nullptr;
}

void writeSampleFile(const std::filesystem::path& path, const SampleSpec& spec)
{
    Writer writer(toUtf8(path));
    if (spec.video)
        writer.addVideo(spec.gopSize);
    if (spec.audio)
        writer.addAudio();
    writer.write(spec.frames);
}

}  // namespace editor::tests
