#include "EditorCore/VideoDecoder.h"

#include "EditorCore/Proxy.h"

#include "EditorCore/FfmpegUtil.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

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

// Fotogrammi già allocati, da riusare: alla risoluzione di un video ogni fotogramma è grande (8 MB in 1080p).
// Un fotogramma torna qui solo quando nessuno lo usa più (l'ultimo shared_ptr, anche nel thread dell'interfaccia),
// quindi il riuso non tocca mai un'immagine ancora in coda o sullo schermo.
class FramePool : public std::enable_shared_from_this<FramePool>
{
public:
    static constexpr std::size_t kKept = 4;  // fotogrammi liberi tenuti da parte, gli altri si liberano

    std::shared_ptr<VideoFrame> acquire()
    {
        std::unique_ptr<VideoFrame> frame;
        {
            std::lock_guard lock(mutex_);
            if (!free_.empty()) {
                frame = std::move(free_.back());
                free_.pop_back();
            }
        }
        if (!frame)
            frame = std::make_unique<VideoFrame>();
        // Il fotogramma torna al pool se c'è ancora (il decoder può essere già stato chiuso).
        return std::shared_ptr<VideoFrame>(frame.release(), [pool = weak_from_this()](VideoFrame* f) {
            std::unique_ptr<VideoFrame> owned(f);
            if (const auto alive = pool.lock())
                alive->release(std::move(owned));
        });
    }

private:
    void release(std::unique_ptr<VideoFrame> frame)
    {
        std::lock_guard lock(mutex_);
        if (free_.size() < kKept)
            free_.push_back(std::move(frame));
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<VideoFrame>> free_;
};

// Parametri con cui è stata impostata la conversione dei colori: se non cambiano non si reimposta.
struct ColorSetup
{
    int sourceWidth = 0;
    int sourceHeight = 0;
    int sourceFormat = -1;
    int width = 0;
    int height = 0;
    bool bt709 = false;
    int fullRange = 0;
    bool operator==(const ColorSetup&) const = default;
};

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
    std::optional<ColorSetup> colorSetup;  // vuoto = da impostare (contesto nuovo)
    std::shared_ptr<FramePool> pool = std::make_shared<FramePool>();

    double fps = 0.0;
    double displayWidth = 0.0;  // larghezza con il rapporto d'aspetto dei pixel applicato
    double displayHeight = 0.0;
    // Dimensioni di uscita impacchettate (larghezza << 32 | altezza): così si leggono e scrivono in un colpo solo.
    std::atomic<std::uint64_t> outSize{0};
    double lastTime = 0.0;

    // Cache dei pacchetti compressi letti dal file (solo il video), in ordine di lettura: si legge in anticipo fino a
    // kCacheAhead secondi oltre l'ultimo fotogramma decodificato e si tengono kCacheBehind secondi già passati (a
    // gruppi interi, da un fotogramma chiave all'altro). Un salto dentro la cache riparte da lì senza rileggere il file.
    struct CachedPacket
    {
        PacketPtr packet;
        double time = 0.0;  // tempo mostrato (pts), in secondi come i fotogrammi
        bool key = false;
    };
    static constexpr double kCacheAhead = 10.0;
    static constexpr double kCacheBehind = 5.0;
    static constexpr std::size_t kCacheMaxBytes = std::size_t{200} << 20;
    static constexpr int kReadAheadPerFrame = 6;  // pacchetti letti in anticipo a ogni fotogramma, al massimo
    std::deque<CachedPacket> cache;
    std::size_t cursor = 0;      // prossimo pacchetto della cache da dare al decoder
    std::size_t cacheBytes = 0;
    double cacheEnd = 0.0;       // tempo più alto fra i pacchetti della cache
    bool demuxerAtEnd = false;   // il file è stato letto fino in fondo (dopo l'ultimo pacchetto della cache)
    bool drainSent = false;      // al decoder è già stato detto che i pacchetti sono finiti
    bool readAheadEnabled = true;
    // Stato del decoder per i salti in avanti: dopo un salto decodifica di seguito da un fotogramma chiave, e
    // lastDecoded è il tempo dell'ultimo fotogramma uscito (anche di quelli saltati per arrivare al punto chiesto).
    bool decoderLive = false;
    double lastDecoded = -1e300;
    // Durante un salto i pacchetti mostrati prima di questo tempo si decodificano senza i fotogrammi che non servono
    // da riferimento agli altri (non si vedono e nessuno ne ha bisogno): si arriva prima al punto chiesto.
    double skipBefore = -1e300;
    std::atomic<double> rangeFrom{0.0};  // vedi cachedRange(): letti anche da un altro thread
    std::atomic<double> rangeTo{0.0};

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
        decoderLive = false;

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
            if (ret == 0) {
                lastDecoded = frameTime(frame.get());
                return true;
            }
            if (ret == AVERROR_EOF)
                return false;
            if (ret != AVERROR(EAGAIN)) {
                decodeError();
                continue;  // fotogramma danneggiato: proviamo con il successivo
            }

            if (cursor >= cache.size() && !readPacket()) {
                if (drainSent)
                    return false;
                drainSent = true;
                avcodec_send_packet(ctx.get(), nullptr);  // svuota il decoder
                continue;
            }
            const AVDiscard discard = cache[cursor].time < skipBefore ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            if (ctx->skip_frame != discard)
                ctx->skip_frame = discard;
            const int sent = avcodec_send_packet(ctx.get(), cache[cursor].packet.get());
            ++cursor;
            if (sent < 0 && sent != AVERROR(EAGAIN))
                decodeError();
            readAhead();
        }
    }

    // Legge dal file il prossimo pacchetto del video e lo mette in fondo alla cache. false a fine file.
    bool readPacket()
    {
        if (demuxerAtEnd)
            return false;
        for (;;) {
            if (av_read_frame(fmt.get(), packet.get()) < 0) {
                demuxerAtEnd = true;
                publishRange();
                return false;
            }
            if (packet->stream_index == index)
                break;
            av_packet_unref(packet.get());
        }
        CachedPacket cached;
        cached.packet.reset(av_packet_alloc());
        if (!cached.packet)
            throw MediaError("Memoria insufficiente per il decoder video.");
        av_packet_move_ref(cached.packet.get(), packet.get());
        const std::int64_t ts = cached.packet->pts != AV_NOPTS_VALUE ? cached.packet->pts : cached.packet->dts;
        cached.time = ts != AV_NOPTS_VALUE ? ts * timeBase - startOffset : cacheEnd;
        cached.key = (cached.packet->flags & AV_PKT_FLAG_KEY) != 0;
        cacheBytes += static_cast<std::size_t>(std::max(0, cached.packet->size));
        cacheEnd = cache.empty() ? cached.time : std::max(cacheEnd, cached.time);
        cache.push_back(std::move(cached));
        publishRange();
        return true;
    }

    // Lettura anticipata (pochi pacchetti alla volta, così ogni fotogramma resta veloce) e pulizia di quello che è
    // ormai troppo indietro.
    void readAhead()
    {
        trimCache();
        for (int i = 0; readAheadEnabled && i < kReadAheadPerFrame && cacheBytes < kCacheMaxBytes && cacheEnd < lastTime + kCacheAhead; ++i) {
            if (!readPacket())
                break;
        }
    }

    void trimCache()
    {
        // Si toglie un gruppo intero (dal primo fotogramma chiave al successivo) quando il decoder l'ha passato e il
        // fotogramma chiave dopo è già più indietro di kCacheBehind (o la cache è troppo grande).
        for (;;) {
            std::size_t next = 1;
            while (next < cache.size() && !cache[next].key)
                ++next;
            if (next >= cache.size() || next >= cursor)
                break;
            const bool old = cache[next].time <= lastTime - kCacheBehind;
            if (!old && cacheBytes <= kCacheMaxBytes)
                break;
            for (std::size_t i = 0; i < next; ++i)
                cacheBytes -= static_cast<std::size_t>(std::max(0, cache[i].packet->size));
            cache.erase(cache.begin(), cache.begin() + static_cast<std::ptrdiff_t>(next));
            cursor -= next;
        }
        publishRange();
    }

    void clearCache()
    {
        cache.clear();
        cursor = 0;
        cacheBytes = 0;
        cacheEnd = 0.0;
        demuxerAtEnd = false;
        publishRange();
    }

    // Tratto decodificabile dalla cache: dal primo fotogramma chiave all'ultimo pacchetto letto.
    void publishRange()
    {
        const auto key = std::find_if(cache.begin(), cache.end(), [](const CachedPacket& c) { return c.key; });
        if (key == cache.end()) {
            rangeFrom = 0.0;
            rangeTo = 0.0;
            return;
        }
        rangeFrom = std::max(0.0, key->time);
        rangeTo = cacheEnd + (fps > 0 ? 1.0 / fps : 0.0);
    }

    // Pacchetto della cache da cui ripartire per arrivare a `target`: l'ultimo fotogramma chiave non dopo `target`.
    // std::nullopt se `target` non è nella cache (si legge dal file).
    std::optional<std::size_t> cachedStart(double target, double tolerance) const
    {
        if (cache.empty() || (!demuxerAtEnd && target > cacheEnd + tolerance))
            return std::nullopt;
        std::optional<std::size_t> start;
        for (std::size_t i = 0; i < cache.size(); ++i) {
            if (cache[i].key && cache[i].time <= target + tolerance)
                start = i;
        }
        return start;
    }

    double frameTime(const AVFrame* f) const
    {
        const std::int64_t ts = frameTimestamp(f);
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
        if (!sws) {
            colorSetup.reset();
            throw MediaError("Conversione dei colori non supportata per questo video.");
        }

        // Colori corretti: BT.709 per l'HD (anche quando il file non lo dichiara), BT.601 altrimenti.
        const bool bt709 = f->colorspace == AVCOL_SPC_BT709
                        || (f->colorspace == AVCOL_SPC_UNSPECIFIED && source->height >= 720);
        const int fullRange = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        // Si imposta solo con un contesto nuovo (cambiano dimensioni o formato) o colori diversi dal fotogramma prima.
        const ColorSetup setup{source->width, source->height, source->format, outWidth, outHeight, bt709, fullRange};
        if (colorSetup != setup) {
            const int* coefficients = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
            sws_setColorspaceDetails(sws.get(), coefficients, fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                     1 << 16, 1 << 16);
            colorSetup = setup;
        }

        // Ogni byte del fotogramma viene riscritto da sws_scale (righe di outWidth * 4 byte, senza margine).
        std::shared_ptr<VideoFrame> out = pool->acquire();
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
        // Il fotogramma "più vicino": il primo che inizia entro mezzo fotogramma dal tempo richiesto.
        const double tolerance = fps > 0 ? 0.5 / fps : 0.001;
        const auto start = cachedStart(target, tolerance);
        // Avanti nello stesso gruppo (il suo fotogramma chiave è già passato dal decoder e il punto non è ancora
        // uscito): si prosegue da dove si è, senza ripartire dal fotogramma chiave.
        const bool goOn = start && decoderLive && !drainSent && *start < cursor && target <= cacheEnd
                       && lastDecoded < target - tolerance;
        if (!goOn) {
            if (start) {
                cursor = *start;  // già in memoria: niente lettura dal file
            } else {
                clearCache();
                seekStream(fmt.get(), index, target, startOffset);
            }
            avcodec_flush_buffers(ctx.get());
            drainSent = false;
            lastDecoded = -1e300;
        }
        decoderLive = true;
        av_frame_unref(previous.get());

        // Solo fino al punto chiesto, anche se si esce con un'eccezione: dopo si decodifica tutto.
        struct SkipUntil
        {
            double& value;
            ~SkipUntil() { value = -1e300; }
        } skip{skipBefore};
        skipBefore = target - tolerance;
        for (;;) {
            if (!decodeNext())
                break;
            if (frameTime(frame.get()) >= target - tolerance) {
                av_frame_unref(previous.get());
                skipBefore = -1e300;
                return convert(frame.get());
            }
            av_frame_unref(previous.get());
            av_frame_move_ref(previous.get(), frame.get());
        }
        skipBefore = -1e300;
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
        colorSetup.reset();
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
    d.readAheadEnabled = options.readAhead;
    if (!d.packet || !d.frame || !d.previous || !d.transferred)
        throw MediaError("Memoria insufficiente per il decoder video.");

    static_cast<OpenedStream&>(d) = openMainStream(utf8Path, AVMEDIA_TYPE_VIDEO);
    // Proxy (vedi Proxy.h): i tempi partono dall'inizio dell'originale, non da quello del proxy.
    if (const AVDictionaryEntry* start = av_dict_get(d.fmt->metadata, kProxyStartTag, nullptr, 0))
        d.startOffset = std::strtod(start->value, nullptr);
    d.hwPixelFormat = d3d11PixelFormat(d.codec);

    d.fps = guessFrameRate(d.fmt.get(), d.stream);

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

std::pair<double, double> VideoDecoder::cachedRange() const
{
    return {impl_->rangeFrom.load(), impl_->rangeTo.load()};
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
