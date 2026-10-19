#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <stdexcept>
#include <string>

namespace editor::tests {

namespace {

constexpr int kFps = 25;

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

    void addVideo(const SampleSpec& spec)
    {
        const AVCodec* codec = avcodec_find_encoder(spec.mpeg2 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_MPEG4);
        video_.enc = avcodec_alloc_context3(codec);
        video_.enc->width = spec.width;
        video_.enc->height = spec.height;
        video_.enc->pix_fmt = AV_PIX_FMT_YUV420P;
        video_.enc->time_base = {1, kFps};
        video_.enc->framerate = {kFps, 1};
        video_.enc->gop_size = spec.gopSize;
        video_.enc->max_b_frames = 0;
        // Qualità alta e costante: i test confrontano i colori decodificati con quelli originali.
        video_.enc->flags |= AV_CODEC_FLAG_QSCALE;
        video_.enc->global_quality = FF_QP2LAMBDA * 2;
        // Niente fotogrammi chiave extra sui cambi di scena: vogliamo esattamente uno ogni gopSize.
        av_opt_set_int(video_.enc, "sc_threshold", 1'000'000'000, AV_OPT_SEARCH_CHILDREN);
        open(video_, codec);
    }

    void addAudio(bool silent, bool aac, int sampleRate)
    {
        silent_ = silent;
        rate_ = sampleRate;
        const AVCodec* codec = avcodec_find_encoder(aac ? AV_CODEC_ID_AAC : AV_CODEC_ID_PCM_S16LE);
        audio_.enc = avcodec_alloc_context3(codec);
        audio_.enc->sample_fmt = aac ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
        audio_.enc->sample_rate = rate_;
        if (aac)
            audio_.enc->bit_rate = 128000;
        const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(av_channel_layout_copy(&audio_.enc->ch_layout, &stereo), "av_channel_layout_copy");
        audio_.enc->time_base = {1, rate_};
        open(audio_, codec);
    }

    void write(int frames)
    {
        if ((fmt_->oformat->flags & AVFMT_NOFILE) == 0)
            check(avio_open(&fmt_->pb, path_.c_str(), AVIO_FLAG_WRITE), "avio_open");
        check(avformat_write_header(fmt_, nullptr), "avformat_write_header");

        // L'audio va in blocchi della misura chiesta dall'encoder (AAC: 1024 campioni), il PCM in blocchi da un
        // fotogramma video; lo si scrive man mano che il video avanza, così i pacchetti restano mescolati.
        const long long totalSamples = static_cast<long long>(frames) * samplesPerFrame();
        const int chunk = audio_.enc != nullptr && audio_.enc->frame_size > 0 ? audio_.enc->frame_size : samplesPerFrame();
        long long written = 0;
        for (int i = 0; i < frames; ++i) {
            if (video_.enc != nullptr)
                encode(video_, makeVideoFrame(i));
            const long long until = std::min(totalSamples, static_cast<long long>(i + 1) * samplesPerFrame());
            while (audio_.enc != nullptr && written < until) {
                const int count = static_cast<int>(std::min<long long>(chunk, totalSamples - written));
                encode(audio_, makeAudioFrame(written, count));
                written += count;
            }
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
        const int width = video_.enc->width;
        const int height = video_.enc->height;
        av_frame_unref(frame_);
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width = width;
        frame_->height = height;
        check(av_frame_get_buffer(frame_, 0), "av_frame_get_buffer");
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                frame_->data[0][y * frame_->linesize[0] + x] = static_cast<uint8_t>(sampleLuma(x, y, firstIndex_ + index));
        for (int plane = 1; plane < 3; ++plane)
            for (int y = 0; y < height / 2; ++y)
                for (int x = 0; x < width / 2; ++x)
                    frame_->data[plane][y * frame_->linesize[plane] + x] = 128;
        frame_->pts = index;
        return frame_;
    }

    AVFrame* makeAudioFrame(long long first, int count)
    {
        av_frame_unref(frame_);
        frame_->format = audio_.enc->sample_fmt;
        frame_->sample_rate = rate_;
        frame_->nb_samples = count;
        check(av_channel_layout_copy(&frame_->ch_layout, &audio_.enc->ch_layout), "av_channel_layout_copy");
        check(av_frame_get_buffer(frame_, 0), "av_frame_get_buffer");
        for (int s = 0; s < count; ++s) {
            const std::int16_t value = silent_ ? 0 : sampleAudio(static_cast<long long>(firstIndex_) * samplesPerFrame() + first + s);
            if (audio_.enc->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                // AAC: float, un piano per canale
                reinterpret_cast<float*>(frame_->data[0])[s] = value / 32768.0f;
                reinterpret_cast<float*>(frame_->data[1])[s] = value / 32768.0f;
            } else {
                auto* samples = reinterpret_cast<int16_t*>(frame_->data[0]);
                samples[2 * s] = value;
                samples[2 * s + 1] = value;
            }
        }
        frame_->pts = first;
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
    bool silent_ = false;
    int rate_ = 48000;
    int firstIndex_ = 0;  // il contenuto parte da questo fotogramma (e dal campione corrispondente)

    int samplesPerFrame() const { return rate_ / kFps; }

public:
    void setFirstIndex(int index) { firstIndex_ = index; }

private:
    Track video_;
    Track audio_;
};

}  // namespace

int sampleLuma(int x, int y, int index)
{
    // Ogni fotogramma è più chiaro del precedente di 5 livelli (dal colore si risale al numero del fotogramma);
    // dopo 36 fotogrammi si ricomincia, per restare fra i valori validi della luminanza.
    return 40 + (index * 5) % 180 + (x + y) / 16;
}

std::int16_t sampleAudio(long long n)
{
    return static_cast<std::int16_t>((n * 7919) % 20000 - 10000);
}

std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool sampleEncodersAvailable()
{
    return avcodec_find_encoder(AV_CODEC_ID_MPEG4) != nullptr && avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO) != nullptr
        && avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE) != nullptr && avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr;
}

void writeSampleFile(const std::filesystem::path& path, const SampleSpec& spec)
{
    Writer writer(toUtf8(path));
    if (spec.video)
        writer.addVideo(spec);
    writer.setFirstIndex(spec.firstIndex);
    if (spec.audio)
        writer.addAudio(spec.silentAudio, spec.aacAudio, spec.sampleRate);
    writer.write(spec.frames);
}

}  // namespace editor::tests
