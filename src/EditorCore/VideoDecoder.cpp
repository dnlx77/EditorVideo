#include "EditorCore/VideoDecoder.h"

#include "EditorCore/FfmpegUtil.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace editor::core {

using namespace detail;

namespace {

// La GPU ha dato errore: si riparte in software dallo stesso punto.
struct HardwareFailure
{
};

struct BufferRefDeleter
{
    void operator()(AVBufferRef* ref) const { av_buffer_unref(&ref); }
};
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

struct SwsDeleter
{
    void operator()(SwsContext* ctx) const { sws_freeContext(ctx); }
};

// Formato pixel con cui il decoder restituisce le superfici D3D11, se il codec supporta D3D11VA.
AVPixelFormat d3d11PixelFormat(const AVCodec* codec)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (config == nullptr)
            return AV_PIX_FMT_NONE;
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
            return config->pix_fmt;
    }
}

}  // namespace

// Il file aperto (fmt, stream, index, codec, timeBase, startOffset) viene da OpenedStream.
struct VideoDecoder::Impl : OpenedStream
{
    CodecContextPtr ctx;
    BufferRefPtr hwDevice;
    AVPixelFormat hwPixelFormat = AV_PIX_FMT_NONE;
    bool hardwareAllowed = true;
    bool hardwareActive = false;

    PacketPtr packet{av_packet_alloc()};
    FramePtr frame{av_frame_alloc()};
    FramePtr previous{av_frame_alloc()};
    FramePtr transferred{av_frame_alloc()};
    std::unique_ptr<SwsContext, SwsDeleter> sws;

    double fps = 0.0;
    double displayWidth = 0.0;  // larghezza con il rapporto d'aspetto dei pixel applicato
    double displayHeight = 0.0;
    // Dimensioni di uscita impacchettate (larghezza << 32 | altezza): così si leggono e scrivono in un colpo solo.
    std::atomic<std::uint64_t> outSize{0};
    bool demuxerAtEnd = false;
    double lastTime = 0.0;

    static AVPixelFormat getFormat(AVCodecContext* c, const AVPixelFormat* formats)
    {
        auto* self = static_cast<Impl*>(c->opaque);
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == self->hwPixelFormat) {
                self->hardwareActive = true;
                return *p;
            }
        }
        // La GPU non gestisce questo flusso (es. profilo non supportato): primo formato software disponibile.
        self->hardwareActive = false;
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
            if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
                return *p;
        }
        return AV_PIX_FMT_NONE;
    }

    void openCodec()
    {
        ctx.reset(avcodec_alloc_context3(codec));
        if (!ctx)
            throw MediaError("Memoria insufficiente per il decoder video.");
        if (avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0)
            throw MediaError("Parametri del flusso video non validi.");
        ctx->pkt_timebase = stream->time_base;
        hardwareActive = false;

        bool hardware = false;
        if (hardwareAllowed && hwPixelFormat != AV_PIX_FMT_NONE) {
            if (!hwDevice) {
                AVBufferRef* device = nullptr;
                if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0)
                    hwDevice.reset(device);
                else
                    hardwareAllowed = false;  // niente scheda video compatibile: inutile riprovare
            }
            if (hwDevice) {
                ctx->hw_device_ctx = av_buffer_ref(hwDevice.get());
                ctx->get_format = &Impl::getFormat;
                ctx->opaque = this;
                hardware = true;
            }
        }
        // In software usiamo tutti i core; con la GPU un solo thread è la configurazione consigliata da FFmpeg.
        ctx->thread_count = hardware ? 1 : 0;

        if (const int ret = avcodec_open2(ctx.get(), codec, nullptr); ret < 0) {
            if (hardware) {
                hardwareAllowed = false;
                openCodec();
                return;
            }
            throw MediaError("Impossibile aprire il decoder video " + std::string(codec->name) + " ("
                             + ffmpegError(ret) + ").");
        }
    }

    bool usingHardwareDevice() const { return ctx && ctx->hw_device_ctx != nullptr; }

    [[noreturn]] void failHardware()
    {
        throw HardwareFailure{};
    }

    // Decodifica il prossimo fotogramma in `frame`. false a fine file.
    bool decodeNext()
    {
        int errors = 0;
        // Errore di decodifica: con la GPU si passa al software (HardwareFailure), altrimenti si salta, fino a 200.
        const auto decodeError = [&] {
            if (usingHardwareDevice() && hardwareActive)
                failHardware();
            if (++errors > 200)
                throw MediaError("Troppi errori di decodifica: il video potrebbe essere danneggiato.");
        };
        for (;;) {
            const int ret = avcodec_receive_frame(ctx.get(), frame.get());
            if (ret == 0)
                return true;
            if (ret == AVERROR_EOF)
                return false;
            if (ret != AVERROR(EAGAIN)) {
                decodeError();
                continue;  // fotogramma danneggiato: proviamo con il successivo
            }

            if (demuxerAtEnd)
                return false;
            const int read = av_read_frame(fmt.get(), packet.get());
            if (read < 0) {
                demuxerAtEnd = true;
                avcodec_send_packet(ctx.get(), nullptr);  // svuota il decoder
                continue;
            }
            if (packet->stream_index != index) {
                av_packet_unref(packet.get());
                continue;
            }
            const int sent = avcodec_send_packet(ctx.get(), packet.get());
            av_packet_unref(packet.get());
            if (sent < 0 && sent != AVERROR(EAGAIN))
                decodeError();
        }
    }

    double frameTime(const AVFrame* f) const
    {
        const std::int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (ts == AV_NOPTS_VALUE)
            return lastTime + (fps > 0 ? 1.0 / fps : 0.0);
        return ts * timeBase - startOffset;
    }

    std::pair<int, int> outputSize() const
    {
        const std::uint64_t packed = outSize.load();
        return {static_cast<int>(packed >> 32), static_cast<int>(packed & 0xffffffffu)};
    }

    // Dimensioni (pari) del fotogramma che sta nel riquadro mantenendo le proporzioni di visualizzazione.
    void fitInto(int boxWidth, int boxHeight, bool allowUpscale)
    {
        double scale = std::min(boxWidth / displayWidth, boxHeight / displayHeight);
        if (!allowUpscale)
            scale = std::min(scale, 1.0);
        const auto even = [](double v) { return std::max(2, static_cast<int>(std::lround(v / 2)) * 2); };
        const std::uint64_t width = even(displayWidth * scale);
        const std::uint64_t height = even(displayHeight * scale);
        outSize = width << 32 | height;
    }

    std::shared_ptr<VideoFrame> convert(const AVFrame* f)
    {
        const AVFrame* source = f;
        if (f->format == hwPixelFormat && f->hw_frames_ctx != nullptr) {
            av_frame_unref(transferred.get());
            if (av_hwframe_transfer_data(transferred.get(), f, 0) < 0)
                failHardware();
            source = transferred.get();
        }

        const auto [outWidth, outHeight] = outputSize();
        sws.reset(sws_getCachedContext(sws.release(), source->width, source->height,
                                       static_cast<AVPixelFormat>(source->format), outWidth, outHeight,
                                       AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws)
            throw MediaError("Conversione dei colori non supportata per questo video.");

        // Colori corretti: BT.709 per l'HD (anche quando il file non lo dichiara), BT.601 altrimenti.
        const bool bt709 = f->colorspace == AVCOL_SPC_BT709
                        || (f->colorspace == AVCOL_SPC_UNSPECIFIED && source->height >= 720);
        const int* coefficients = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        const int fullRange = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(sws.get(), coefficients, fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                 1 << 16, 1 << 16);

        auto out = std::make_shared<VideoFrame>();
        out->time = frameTime(f);
        out->width = outWidth;
        out->height = outHeight;
        out->stride = outWidth * 4;
        out->pixels.resize(static_cast<std::size_t>(out->stride) * outHeight);
        std::uint8_t* dst[4] = {out->pixels.data(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {out->stride, 0, 0, 0};
        sws_scale(sws.get(), source->data, source->linesize, 0, source->height, dst, dstStride);

        lastTime = out->time;
        return out;
    }

    std::shared_ptr<VideoFrame> seekImpl(double seconds)
    {
        const double target = std::max(0.0, seconds);
        seekStream(fmt.get(), index, target, startOffset);
        avcodec_flush_buffers(ctx.get());
        demuxerAtEnd = false;
        av_frame_unref(previous.get());

        // Il fotogramma "più vicino": il primo che inizia entro mezzo fotogramma dal tempo richiesto.
        const double tolerance = fps > 0 ? 0.5 / fps : 0.001;
        for (;;) {
            if (!decodeNext())
                break;
            if (frameTime(frame.get()) >= target - tolerance) {
                av_frame_unref(previous.get());
                return convert(frame.get());
            }
            av_frame_unref(previous.get());
            av_frame_move_ref(previous.get(), frame.get());
        }
        // Tempo oltre la fine: ultimo fotogramma disponibile.
        if (previous->buf[0] != nullptr || previous->hw_frames_ctx != nullptr)
            return convert(previous.get());
        return nullptr;
    }

    std::shared_ptr<VideoFrame> nextImpl()
    {
        if (!decodeNext())
            return nullptr;
        return convert(frame.get());
    }

    void switchToSoftware()
    {
        hardwareAllowed = false;
        sws.reset();
        openCodec();
    }
};

VideoDecoder::VideoDecoder(const std::string& utf8Path)
    : VideoDecoder(utf8Path, Options{})
{
}

VideoDecoder::VideoDecoder(const std::string& utf8Path, const Options& options)
    : impl_(std::make_unique<Impl>())
{
    Impl& d = *impl_;
    d.hardwareAllowed = options.allowHardware;
    if (!d.packet || !d.frame || !d.previous || !d.transferred)
        throw MediaError("Memoria insufficiente per il decoder video.");

    static_cast<OpenedStream&>(d) = openMainStream(utf8Path, AVMEDIA_TYPE_VIDEO);
    d.hwPixelFormat = d3d11PixelFormat(d.codec);

    const AVRational rate = av_guess_frame_rate(d.fmt.get(), d.stream, nullptr);
    d.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;

    // Dimensioni di visualizzazione: si applica il rapporto d'aspetto dei pixel, poi si rientra nel riquadro massimo.
    const AVCodecParameters* par = d.stream->codecpar;
    if (par->width <= 0 || par->height <= 0)
        throw MediaError("Dimensioni del video sconosciute: " + utf8Path);
    const AVRational sar = av_guess_sample_aspect_ratio(d.fmt.get(), d.stream, nullptr);
    d.displayWidth = par->width;
    d.displayHeight = par->height;
    if (sar.num > 0 && sar.den > 0)
        d.displayWidth = par->width * av_q2d(sar);
    d.fitInto(options.maxWidth, options.maxHeight, false);

    d.openCodec();
}

VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::usingHardware() const
{
    return impl_->usingHardwareDevice() && impl_->hardwareActive;
}

std::string VideoDecoder::decoderDescription() const
{
    return usingHardware() ? "GPU (D3D11VA)" : "software";
}

double VideoDecoder::durationSeconds() const
{
    return durationOf(impl_->fmt.get(), impl_->stream);
}

double VideoDecoder::frameRate() const
{
    return impl_->fps;
}

int VideoDecoder::outputWidth() const
{
    return impl_->outputSize().first;
}

void VideoDecoder::setOutputSize(int boxWidth, int boxHeight)
{
    // Limite di sicurezza: oltre l'8K non serve per un'anteprima.
    impl_->fitInto(std::clamp(boxWidth, 2, 7680), std::clamp(boxHeight, 2, 4320), true);
}

int VideoDecoder::outputHeight() const
{
    return impl_->outputSize().second;
}

std::shared_ptr<VideoFrame> VideoDecoder::seek(double seconds)
{
    try {
        return impl_->seekImpl(seconds);
    } catch (const HardwareFailure&) {
        impl_->switchToSoftware();
        return impl_->seekImpl(seconds);
    }
}

std::shared_ptr<VideoFrame> VideoDecoder::nextFrame()
{
    try {
        return impl_->nextImpl();
    } catch (const HardwareFailure&) {
        // Si riparte in software dal fotogramma mostrato per ultimo e si prosegue con il successivo.
        const double last = impl_->lastTime;
        impl_->switchToSoftware();
        impl_->seekImpl(last);
        return impl_->nextImpl();
    }
}

bool hardwareDecodingCompiledIn()
{
    for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); type != AV_HWDEVICE_TYPE_NONE;
         type = av_hwdevice_iterate_types(type)) {
        if (type == AV_HWDEVICE_TYPE_D3D11VA)
            return true;
    }
    return false;
}

}  // namespace editor::core
