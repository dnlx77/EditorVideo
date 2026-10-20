#include "EditorCore/AudioDecoder.h"

#include "EditorCore/FfmpegUtil.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>

namespace editor::core {

using namespace detail;

namespace {

struct SwrDeleter
{
    void operator()(SwrContext* ctx) const { swr_free(&ctx); }
};

}  // namespace

// Il file aperto (fmt, stream, index, codec, timeBase, startOffset) viene da OpenedStream.
struct AudioDecoder::Impl : OpenedStream
{
    CodecContextPtr ctx;
    std::unique_ptr<SwrContext, SwrDeleter> swr;
    PacketPtr packet{av_packet_alloc()};
    FramePtr frame{av_frame_alloc()};

    // Formato di ingresso con cui è stato configurato swr: se cambia a metà file lo riconfiguriamo.
    AVChannelLayout swrInLayout = {};
    int swrInFormat = -1;
    int swrInRate = 0;

    int outRate = 0;
    int outChannels = 0;
    bool demuxerAtEnd = false;
    double nextTime = 0.0;            // tempo atteso del prossimo campione, se il pacchetto non ha timestamp
    std::optional<double> trimUntil;  // dopo un seek: scarta i campioni prima di questo tempo

    ~Impl() { av_channel_layout_uninit(&swrInLayout); }

    bool decodeNext()
    {
        int errors = 0;
        for (;;) {
            const int ret = avcodec_receive_frame(ctx.get(), frame.get());
            if (ret == 0)
                return true;
            if (ret == AVERROR_EOF)
                return false;
            if (ret != AVERROR(EAGAIN)) {
                if (++errors > 200)
                    throw MediaError("Troppi errori di decodifica: l'audio potrebbe essere danneggiato.");
                continue;
            }

            if (demuxerAtEnd)
                return false;
            const int read = av_read_frame(fmt.get(), packet.get());
            if (read < 0) {
                demuxerAtEnd = true;
                avcodec_send_packet(ctx.get(), nullptr);
                continue;
            }
            if (packet->stream_index == index)
                avcodec_send_packet(ctx.get(), packet.get());
            av_packet_unref(packet.get());
        }
    }

    void configureResampler(const AVFrame* f)
    {
        AVChannelLayout inLayout = {};
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&inLayout, f->ch_layout.nb_channels);
        else
            av_channel_layout_copy(&inLayout, &f->ch_layout);

        if (swr && f->format == swrInFormat && f->sample_rate == swrInRate
            && av_channel_layout_compare(&inLayout, &swrInLayout) == 0) {
            av_channel_layout_uninit(&inLayout);
            return;
        }

        AVChannelLayout outLayout = {};
        av_channel_layout_default(&outLayout, outChannels);
        SwrContext* raw = nullptr;
        const int ret = swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_S16, outRate, &inLayout,
                                            static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        swr.reset(raw);
        av_channel_layout_uninit(&outLayout);
        if (ret < 0 || swr_init(swr.get()) < 0) {
            av_channel_layout_uninit(&inLayout);
            throw MediaError("Formato audio non supportato per l'anteprima.");
        }

        av_channel_layout_uninit(&swrInLayout);
        swrInLayout = inLayout;
        swrInFormat = f->format;
        swrInRate = f->sample_rate;
    }
};

AudioDecoder::AudioDecoder(const std::string& utf8Path)
    : impl_(std::make_unique<Impl>())
{
    Impl& d = *impl_;
    if (!d.packet || !d.frame)
        throw MediaError("Memoria insufficiente per il decoder audio.");

    static_cast<OpenedStream&>(d) = openMainStream(utf8Path, AVMEDIA_TYPE_AUDIO);
    d.ctx.reset(avcodec_alloc_context3(d.codec));
    if (!d.ctx || avcodec_parameters_to_context(d.ctx.get(), d.stream->codecpar) < 0)
        throw MediaError("Parametri del flusso audio non validi.");
    d.ctx->pkt_timebase = d.stream->time_base;
    if (const int ret = avcodec_open2(d.ctx.get(), d.codec, nullptr); ret < 0)
        throw MediaError("Impossibile aprire il decoder audio " + std::string(d.codec->name) + " (" + ffmpegError(ret)
                         + ").");

    if (d.ctx->sample_rate <= 0 || d.ctx->ch_layout.nb_channels <= 0)
        throw MediaError("Parametri del flusso audio sconosciuti: " + utf8Path);
    d.outRate = d.ctx->sample_rate;
    d.outChannels = std::min(2, d.ctx->ch_layout.nb_channels);
}

AudioDecoder::~AudioDecoder() = default;

int AudioDecoder::sampleRate() const
{
    return impl_->outRate;
}

int AudioDecoder::channels() const
{
    return impl_->outChannels;
}

double AudioDecoder::durationSeconds() const
{
    return durationOf(impl_->fmt.get(), impl_->stream);
}

void AudioDecoder::seek(double seconds)
{
    Impl& d = *impl_;
    const double target = std::max(0.0, seconds);
    seekStream(d.fmt.get(), d.index, target, d.startOffset);
    avcodec_flush_buffers(d.ctx.get());
    d.demuxerAtEnd = false;
    d.nextTime = target;
    d.trimUntil = target;
}

std::optional<AudioChunk> AudioDecoder::next()
{
    Impl& d = *impl_;
    while (d.decodeNext()) {
        AVFrame* f = d.frame.get();
        const std::int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        const double time = ts != AV_NOPTS_VALUE ? ts * d.timeBase - d.startOffset : d.nextTime;

        d.configureResampler(f);
        const int capacity = swr_get_out_samples(d.swr.get(), f->nb_samples);
        AudioChunk chunk;
        chunk.samples.resize(static_cast<std::size_t>(std::max(capacity, 0)) * d.outChannels);
        auto* out = reinterpret_cast<std::uint8_t*>(chunk.samples.data());
        const int converted = swr_convert(d.swr.get(), &out, capacity, const_cast<const std::uint8_t**>(f->extended_data),
                                          f->nb_samples);
        av_frame_unref(f);
        if (converted <= 0)
            continue;
        chunk.samples.resize(static_cast<std::size_t>(converted) * d.outChannels);
        chunk.time = time;
        d.nextTime = time + converted / double(d.outRate);

        if (d.trimUntil) {
            // Dopo un seek il demuxer riparte da un punto precedente: tagliamo fino al tempo richiesto.
            if (d.nextTime <= *d.trimUntil)
                continue;
            const auto skip = static_cast<std::size_t>(std::max(0.0, std::round((*d.trimUntil - time) * d.outRate)));
            if (skip > 0) {
                chunk.samples.erase(chunk.samples.begin(), chunk.samples.begin() + skip * d.outChannels);
                chunk.time = time + skip / double(d.outRate);
            }
            d.trimUntil.reset();
            if (chunk.samples.empty())
                continue;
        }
        return chunk;
    }
    return std::nullopt;
}

}  // namespace editor::core
