#include "EditorCore/Proxy.h"

#include "EditorCore/Encoder.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/MediaError.h"
#include "EditorCore/PathUtil.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <set>
#include <system_error>

namespace editor::core {

using namespace detail;

namespace {

// FNV-1a a 64 bit: un codice stabile (lo stesso a ogni avvio) per riconoscere l'originale.
std::uint64_t fnv1a(const std::string& text)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

struct OutputDeleter
{
    void operator()(AVFormatContext* fmt) const
    {
        if (fmt == nullptr)
            return;
        if (fmt->pb != nullptr && (fmt->oformat->flags & AVFMT_NOFILE) == 0)
            avio_closep(&fmt->pb);
        avformat_free_context(fmt);
    }
};

// La GPU NVIDIA ha dato errore: si rifà il proxy da capo con il processore.
struct GpuFailure
{
};

// Formato in memoria normale (il primo che non è della scheda video): il decoder NVIDIA copia lui i fotogrammi, già
// ridotti, dalla GPU.
AVPixelFormat softwareFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

// Decoder della scheda video NVIDIA (NVDEC, "<codec>_cuvid") che riduce i fotogrammi alle misure del proxy già sulla
// GPU. Vuoto se non c'è (niente scheda NVIDIA, codec o profilo non gestito). Solo per i codec delle videocamere e dei
// telefoni: con quelli vecchi (es. MPEG-4 Part 2) NVDEC non dà sempre gli stessi fotogrammi del decoder normale.
CodecContextPtr openGpuDecoder(const AVCodec* codec, const AVStream* stream, int width, int height)
{
    switch (codec->id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_AV1:
    case AV_CODEC_ID_VP9:
        break;
    default:
        return {};
    }
    const AVCodec* gpu = avcodec_find_decoder_by_name((std::string(codec->name) + "_cuvid").c_str());
    if (gpu == nullptr)
        return {};
    CodecContextPtr ctx(avcodec_alloc_context3(gpu));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0)
        return {};
    ctx->pkt_timebase = stream->time_base;
    ctx->get_format = &softwareFormat;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "resize", (std::to_string(width) + "x" + std::to_string(height)).c_str(), 0);
    const int ret = avcodec_open2(ctx.get(), gpu, &options);
    av_dict_free(&options);
    return ret < 0 ? CodecContextPtr() : std::move(ctx);
}

ProxyInfo buildProxyWith(const std::string& originalUtf8Path, const std::string& proxyUtf8Path, int divisor,
                         const std::function<void(double)>& progress, const std::atomic<bool>* cancel, bool gpu);

}  // namespace

std::string proxyFileName(const std::string& originalUtf8Path, int divisor)
{
    namespace fs = std::filesystem;
    const fs::path original = pathFromUtf8(originalUtf8Path);
    std::error_code ec;
    const auto size = fs::file_size(original, ec);
    if (ec)
        return {};
    const auto modified = fs::last_write_time(original, ec);
    if (ec)
        return {};
    const auto ticks = modified.time_since_epoch().count();
    const std::string key = sameFileKey(originalUtf8Path) + "|" + std::to_string(size) + "|" + std::to_string(ticks);
    char code[17];
    std::snprintf(code, sizeof code, "%016llx", static_cast<unsigned long long>(fnv1a(key)));
    return utf8FromPath(original.stem()) + "_" + code + "_1-" + std::to_string(divisor) + ".mp4";
}

std::pair<int, int> proxySize(double displayWidth, double displayHeight, int divisor)
{
    const auto even = [&](double v) { return std::max(2, static_cast<int>(std::lround(v / std::max(1, divisor) / 2)) * 2); };
    return {even(displayWidth), even(displayHeight)};
}

ProxyInfo buildProxy(const std::string& originalUtf8Path, const std::string& proxyUtf8Path, int divisor,
                     const std::function<void(double)>& progress, const std::atomic<bool>* cancel, bool allowGpu)
{
    if (allowGpu) {
        try {
            return buildProxyWith(originalUtf8Path, proxyUtf8Path, divisor, progress, cancel, true);
        } catch (const GpuFailure&) {
            // Niente GPU NVIDIA, o ha dato errore a metà: tutto con il processore.
        }
    }
    return buildProxyWith(originalUtf8Path, proxyUtf8Path, divisor, progress, cancel, false);
}

namespace {

ProxyInfo buildProxyWith(const std::string& originalUtf8Path, const std::string& proxyUtf8Path, int divisor,
                         const std::function<void(double)>& progress, const std::atomic<bool>* cancel, bool gpu)
{
    namespace fs = std::filesystem;
    OpenedStream in = openMainStream(originalUtf8Path, AVMEDIA_TYPE_VIDEO);
    AVStream* stream = in.stream;
    const double duration = durationOf(in.fmt.get(), stream);

    // Misure: quelle di visualizzazione (proporzioni dei pixel applicate), divise.
    const AVCodecParameters* par = stream->codecpar;
    const AVRational sar = av_guess_sample_aspect_ratio(in.fmt.get(), stream, nullptr);
    const double displayWidth = sar.num > 0 && sar.den > 0 ? par->width * av_q2d(sar) : par->width;
    const auto [width, height] = proxySize(displayWidth, par->height, divisor);

    // Con la GPU NVIDIA decodifica e riduzione si fanno sulla scheda video; altrimenti con il processore (tutti i core)
    // e la riduzione la fa l'encoder.
    CodecContextPtr decoder;
    if (gpu) {
        decoder = openGpuDecoder(in.codec, stream, width, height);
        if (!decoder)
            throw GpuFailure{};
    } else {
        decoder.reset(avcodec_alloc_context3(in.codec));
        if (!decoder || avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0)
            throw MediaError("Decoder video non disponibile per il proxy.");
        decoder->pkt_timebase = stream->time_base;
        decoder->thread_count = 0;  // tutti i core
        if (const int ret = avcodec_open2(decoder.get(), in.codec, nullptr); ret < 0)
            throw MediaError("Impossibile aprire il decoder video per il proxy (" + ffmpegError(ret) + ").");
    }
    AVRational rate = av_guess_frame_rate(in.fmt.get(), stream, nullptr);
    if (rate.num <= 0 || rate.den <= 0)
        rate = {25, 1};
    const std::int64_t frameTicks = std::max<std::int64_t>(1, av_rescale_q(1, av_inv_q(rate), stream->time_base));

    VideoEncoderSettings settings;
    settings.codec = VideoCodec::H264;
    settings.width = width;
    settings.height = height;
    settings.frameRate = {rate.num, rate.den};
    settings.timeBase = {stream->time_base.num, stream->time_base.den};
    settings.quality = QualityPreset::Medium;
    settings.gopFrames = kProxyKeyframeInterval;
    settings.bFrames = 0;
    VideoEncoder encoder(settings);

    // Si scrive "<proxy>.part" (formato detto a parte: l'estensione non è quella del contenitore).
    const fs::path finalPath = pathFromUtf8(proxyUtf8Path);
    fs::path partPath = finalPath;
    partPath += ".part";
    const std::string partUtf8 = utf8FromPath(partPath);
    std::error_code ec;
    fs::create_directories(finalPath.parent_path(), ec);

    AVFormatContext* raw = nullptr;
    if (avformat_alloc_output_context2(&raw, nullptr, "mp4", partUtf8.c_str()) < 0 || raw == nullptr)
        throw MediaError("Impossibile creare il file del proxy.");
    std::unique_ptr<AVFormatContext, OutputDeleter> out(raw);
    AVStream* outStream = avformat_new_stream(out.get(), nullptr);
    if (outStream == nullptr)
        throw MediaError("Memoria insufficiente per il proxy.");
    encoder.copyParameters(outStream->codecpar);
    outStream->time_base = stream->time_base;
    outStream->avg_frame_rate = rate;
    char start[64];
    std::snprintf(start, sizeof start, "%.9f", in.startOffset);
    av_dict_set(&out->metadata, kProxyStartTag, start, 0);

    bool written = false;
    const auto removePart = [&] {
        if (out && out->pb != nullptr)
            avio_closep(&out->pb);
        std::error_code ignored;
        fs::remove(partPath, ignored);
    };
    try {
        if (const int ret = avio_open(&out->pb, partUtf8.c_str(), AVIO_FLAG_WRITE); ret < 0)
            throw MediaError("Impossibile scrivere il proxy in " + proxyUtf8Path + " (" + ffmpegError(ret) + ").");
        AVDictionary* options = nullptr;
        av_dict_set(&options, "movflags", "use_metadata_tags", 0);
        const int header = avformat_write_header(out.get(), &options);
        av_dict_free(&options);
        if (header < 0)
            throw MediaError("Impossibile scrivere il proxy (" + ffmpegError(header) + ").");
        written = true;

        // Senza B-frame i pacchetti escono nell'ordine dei fotogrammi: a ognuno si dà il tempo esatto del fotogramma
        // originale corrispondente (gli encoder della scheda video possono arrotondare i tempi).
        std::deque<std::int64_t> times;
        std::int64_t lastTime = AV_NOPTS_VALUE;
        // Tempi dei pacchetti che il file dice di non mostrare (es. prima dell'inizio negli MP4 con le liste di
        // modifica): il decoder normale li scarta da solo, quello NVIDIA no.
        std::set<std::int64_t> discarded;
        const AVRational encoderTb{encoder.timeBase().num, encoder.timeBase().den};
        const PacketSink sink = [&](AVPacket* packet) {
            if (times.empty())
                return;  // non dovrebbe succedere: un pacchetto in più dell'encoder
            const std::int64_t t = times.front();
            times.pop_front();
            packet->pts = packet->dts = av_rescale_q(t, stream->time_base, outStream->time_base);
            packet->duration = av_rescale_q(frameTicks, stream->time_base, outStream->time_base);
            packet->stream_index = outStream->index;
            if (const int ret = av_interleaved_write_frame(out.get(), packet); ret < 0)
                throw MediaError("Errore scrivendo il proxy (" + ffmpegError(ret) + ").");
        };

        PacketPtr packet(av_packet_alloc());
        FramePtr frame(av_frame_alloc());
        if (!packet || !frame)
            throw MediaError("Memoria insufficiente per il proxy.");
        int count = 0;
        const auto encodeFrame = [&] {
            if (gpu)
                frame->sample_aspect_ratio = {1, 1};  // già ridotto alle misure di visualizzazione
            std::int64_t t = frameTimestamp(frame.get());
            if (t == AV_NOPTS_VALUE)
                t = lastTime == AV_NOPTS_VALUE ? 0 : lastTime + frameTicks;
            if (discarded.contains(t))
                return;
            if (lastTime != AV_NOPTS_VALUE && t <= lastTime)
                return;  // tempo ripetuto o all'indietro: il decoder l'ha già dato (l'originale lo mostrerebbe uguale)
            lastTime = t;
            times.push_back(t);
            frame->pts = av_rescale_q(t, stream->time_base, encoderTb);
            encoder.encode(frame.get(), count % kProxyKeyframeInterval == 0, sink);
            ++count;
            if (progress && duration > 0)
                progress(std::clamp((t * in.timeBase - in.startOffset) / duration, 0.0, 1.0));
        };
        const auto receive = [&] {
            for (;;) {
                const int ret = avcodec_receive_frame(decoder.get(), frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    return;
                if (ret < 0) {
                    if (gpu)
                        throw GpuFailure{};
                    continue;  // fotogramma danneggiato: si salta
                }
                encodeFrame();
                av_frame_unref(frame.get());
            }
        };
        while (av_read_frame(in.fmt.get(), packet.get()) >= 0) {
            if (isCancelled(cancel))
                throw MediaError("Creazione del proxy interrotta.");
            if (packet->stream_index == in.index) {
                if ((packet->flags & AV_PKT_FLAG_DISCARD) != 0 && packet->pts != AV_NOPTS_VALUE)
                    discarded.insert(packet->pts);
                const int sent = avcodec_send_packet(decoder.get(), packet.get());  // un pacchetto danneggiato si salta
                if (sent < 0 && sent != AVERROR(EAGAIN) && gpu)
                    throw GpuFailure{};
                receive();
            }
            av_packet_unref(packet.get());
        }
        avcodec_send_packet(decoder.get(), nullptr);
        receive();
        encoder.finish(sink);
        if (count == 0)
            throw MediaError("Nessun fotogramma decodificato: proxy non creato.");
        if (const int ret = av_write_trailer(out.get()); ret < 0)
            throw MediaError("Errore chiudendo il proxy (" + ffmpegError(ret) + ").");
        avio_closep(&out->pb);
    } catch (...) {
        if (written)
            av_write_trailer(out.get());
        removePart();
        throw;
    }

    fs::rename(partPath, finalPath, ec);
    if (ec) {
        fs::remove(finalPath, ec);  // su Windows rename non sovrascrive
        fs::rename(partPath, finalPath, ec);
    }
    if (ec) {
        removePart();
        throw MediaError("Impossibile salvare il proxy: " + ec.message());
    }
    if (progress)
        progress(1.0);
    return {.gpuResize = gpu, .encoder = describe(encoder.info())};
}

}  // namespace

}  // namespace editor::core
