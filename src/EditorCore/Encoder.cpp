#include "EditorCore/Encoder.h"

#include "EditorCore/FfmpegUtil.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <thread>

namespace editor::core {

using namespace detail;

namespace {

// Un encoder da provare: nome in FFmpeg, codec e tipo.
struct Candidate
{
    const char* name;
    VideoCodec codec;
    EncoderBackend backend;
};

// In ordine di preferenza, per ogni codec.
constexpr Candidate kCandidates[] = {
    {"h264_nvenc", VideoCodec::H264, EncoderBackend::Nvenc},
    {"h264_qsv", VideoCodec::H264, EncoderBackend::Qsv},
    {"h264_amf", VideoCodec::H264, EncoderBackend::Amf},
    {"h264_mf", VideoCodec::H264, EncoderBackend::MediaFoundation},
    {"hevc_nvenc", VideoCodec::Hevc, EncoderBackend::Nvenc},
    {"hevc_qsv", VideoCodec::Hevc, EncoderBackend::Qsv},
    {"hevc_amf", VideoCodec::Hevc, EncoderBackend::Amf},
    {"hevc_mf", VideoCodec::Hevc, EncoderBackend::MediaFoundation},
    {"av1_nvenc", VideoCodec::Av1, EncoderBackend::Nvenc},
    {"av1_qsv", VideoCodec::Av1, EncoderBackend::Qsv},
    {"av1_amf", VideoCodec::Av1, EncoderBackend::Amf},
    {"av1_mf", VideoCodec::Av1, EncoderBackend::MediaFoundation},
};

struct FifoDeleter
{
    void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

struct DictionaryDeleter
{
    void operator()(AVDictionary* dict) const { av_dict_free(&dict); }
};

AVRational toAv(Rational r)
{
    return {r.num, r.den};
}

// Formato dei pixel per l'encoder: quello preferito se lo accetta, altrimenti NV12, altrimenti il primo non della
// scheda video.
AVPixelFormat choosePixelFormat(const AVCodec* codec, AVPixelFormat preferred)
{
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 || configs == nullptr)
        return preferred;  // nessun elenco: accetta di tutto
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    const auto accepts = [&](AVPixelFormat f) { return std::find(formats, formats + count, f) != formats + count; };
    if (accepts(preferred))
        return preferred;
    if (accepts(AV_PIX_FMT_NV12))
        return AV_PIX_FMT_NV12;
    for (int i = 0; i < count; ++i) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(formats[i]);
        if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
            return formats[i];
    }
    return preferred;
}

// Preset e scala del CQ di ogni tipo di encoder (vedi videoEncoderTuning).
const std::vector<EncoderPreset> kNvencPresets = {
    {"p1", "p1 (il più veloce)"}, {"p2", "p2"}, {"p3", "p3"}, {"p4", "p4"},
    {"p5", "p5 (consigliato)"},   {"p6", "p6"}, {"p7", "p7 (il più lento, migliore)"},
};
const std::vector<EncoderPreset> kQsvPresets = {
    {"veryfast", "Velocissimo"}, {"faster", "Più veloce"}, {"fast", "Veloce"}, {"medium", "Medio"},
    {"slow", "Lento"},           {"slower", "Più lento"},  {"veryslow", "Lentissimo"},
};
const std::vector<EncoderPreset> kAmfPresets = {
    {"speed", "Velocità"}, {"balanced", "Bilanciato"}, {"quality", "Qualità"},
};

// Qualità Media Foundation (0..100, più = migliore) per un CQ (meno = migliore): una scala approssimata, solo perché il
// cursore del CQ funzioni anche con questo encoder.
int mediaFoundationQuality(int cq)
{
    return std::clamp(100 - cq * 3 / 2, 1, 100);
}

// Opzioni di qualità costante per ogni tipo di encoder. Preset e CQ di `s`, se ci sono, valgono più di `s.quality`.
void qualityOptions(AVCodecContext* ctx, AVDictionary** opts, EncoderBackend backend, VideoCodec codec, const VideoEncoderSettings& s)
{
    const bool high = s.quality == QualityPreset::High;
    const bool customCq = s.cq > 0;
    const std::string number = std::to_string(s.cq);
    switch (backend) {
    case EncoderBackend::Nvenc:
        av_dict_set(opts, "preset", s.preset.empty() ? (high ? "p5" : "p4") : s.preset.c_str(), 0);
        av_dict_set(opts, "tune", "hq", 0);
        av_dict_set(opts, "rc", "vbr", 0);
        av_dict_set(opts, "cq", customCq ? number.c_str() : codec == VideoCodec::Av1 ? (high ? "28" : "34") : (high ? "19" : "25"), 0);
        av_dict_set(opts, "forced-idr", "1", 0);  // i fotogrammi chiave chiesti sono IDR
        ctx->bit_rate = 0;
        break;
    case EncoderBackend::Qsv:
        ctx->global_quality = customCq ? s.cq : high ? 20 : 26;
        if (!s.preset.empty())
            av_dict_set(opts, "preset", s.preset.c_str(), 0);
        av_dict_set(opts, "forced_idr", "1", 0);
        break;
    case EncoderBackend::Amf:
        av_dict_set(opts, "rc", "cqp", 0);
        av_dict_set(opts, "qp_i", customCq ? number.c_str() : high ? "20" : "26", 0);
        av_dict_set(opts, "qp_p", customCq ? std::to_string(s.cq + 2).c_str() : high ? "22" : "28", 0);
        av_dict_set(opts, "quality", s.preset.empty() ? "quality" : s.preset.c_str(), 0);
        break;
    case EncoderBackend::MediaFoundation: {
        av_dict_set(opts, "rate_control", "quality", 0);
        av_dict_set(opts, "quality", customCq ? std::to_string(mediaFoundationQuality(s.cq)).c_str() : high ? "85" : "55", 0);
        // Se il codificatore di Windows non conosce la qualità costante usa questa quantità di dati (come per gli altri
        // encoder, ogni 6 punti di CQ in più la metà).
        const double factor = customCq ? 0.15 * std::exp2((19 - s.cq) / 6.0) : high ? 0.15 : 0.08;
        ctx->bit_rate = static_cast<std::int64_t>(static_cast<double>(ctx->width) * ctx->height * av_q2d(ctx->framerate) * factor);
        break;
    }
    }
}

// Encoder video aperto (senza fotogrammi ancora). Lancia MediaError se non si apre.
CodecContextPtr openVideo(const Candidate& candidate, const VideoEncoderSettings& s)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(candidate.name);
    if (codec == nullptr)
        throw MediaError(std::string("Encoder ") + candidate.name + " non presente in questa versione di FFmpeg.");
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx)
        throw MediaError("Memoria insufficiente per l'encoder video.");
    const Rational rate = s.frameRate.num > 0 && s.frameRate.den > 0 ? s.frameRate : Rational{25, 1};
    ctx->width = s.width;
    ctx->height = s.height;
    ctx->framerate = toAv(rate);
    ctx->time_base = s.timeBase.num > 0 ? toAv(s.timeBase) : AVRational{rate.den, rate.num};
    ctx->sample_aspect_ratio = s.sampleAspectRatio.num > 0 ? toAv(s.sampleAspectRatio) : AVRational{1, 1};
    ctx->pix_fmt = choosePixelFormat(codec, AV_PIX_FMT_YUV420P);
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->gop_size = s.gopFrames > 0 ? s.gopFrames : std::max(1, static_cast<int>(2 * av_q2d(ctx->framerate) + 0.5));
    if (s.referenceFrames > 0)
        ctx->refs = s.referenceFrames;
    ctx->max_b_frames = std::max(0, s.bFrames);  // di solito 0: vedi VideoEncoderSettings::bFrames
    if (!s.inBandParameters)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* raw = nullptr;
    qualityOptions(ctx.get(), &raw, candidate.backend, candidate.codec, s);
    if (s.inBandParameters && candidate.backend == EncoderBackend::Nvenc && candidate.codec != VideoCodec::Av1)
        av_dict_set(&raw, "repeat_headers", "1", 0);  // NVENC: parametri prima di ogni fotogramma chiave (non solo il primo)
    const int ret = avcodec_open2(ctx.get(), codec, &raw);
    std::unique_ptr<AVDictionary, DictionaryDeleter> leftover(raw);
    if (ret < 0)
        throw MediaError(std::string("L'encoder ") + candidate.name + " non si apre: " + ffmpegError(ret));
    return ctx;
}

// Manda un fotogramma (o nullptr = fine) e passa a `sink` i pacchetti pronti. Alcuni encoder (Media Foundation)
// accettano un nuovo fotogramma solo dopo aver dato i pacchetti: si alternano invio e ricezione.
void sendAndDrain(AVCodecContext* ctx, const AVFrame* frame, const PacketSink& sink, AVPacket* packet)
{
    const auto drain = [&](bool untilEnd) {
        for (;;) {
            const int ret = avcodec_receive_packet(ctx, packet);
            if (ret == AVERROR(EAGAIN) && !untilEnd)
                return;
            if (ret == AVERROR_EOF)
                return;
            if (ret == AVERROR(EAGAIN))
                continue;  // a fine flusso si aspetta finché l'encoder non ha dato tutto
            if (ret < 0)
                throw MediaError("Errore dell'encoder: " + ffmpegError(ret));
            if (sink)
                sink(packet);
            av_packet_unref(packet);
        }
    };
    for (;;) {
        const int ret = avcodec_send_frame(ctx, frame);
        if (ret == AVERROR(EAGAIN)) {
            const int got = avcodec_receive_packet(ctx, packet);
            if (got < 0 && got != AVERROR(EAGAIN))
                throw MediaError("Errore dell'encoder: " + ffmpegError(got));
            if (got >= 0) {
                if (sink)
                    sink(packet);
                av_packet_unref(packet);
            }
            continue;
        }
        if (ret < 0 && !(frame == nullptr && ret == AVERROR_EOF))
            throw MediaError("Errore dell'encoder: " + ffmpegError(ret));
        break;
    }
    drain(frame == nullptr);
}

// Come sopra, con un pacchetto usato solo per questa chiamata (per i pochi fotogrammi di una prova).
void sendAndDrain(AVCodecContext* ctx, const AVFrame* frame, const PacketSink& sink)
{
    PacketPtr packet(av_packet_alloc());
    if (!packet)
        throw MediaError("Memoria insufficiente per l'encoder.");
    sendAndDrain(ctx, frame, sink, packet.get());
}

// Fotogramma di lavoro da riusare: la prima volta (o se cambiano formato o misura) alloca i buffer, poi basta renderlo
// scrivibile (se l'encoder tiene ancora un riferimento ai dati, ne prende di nuovi).
void prepareBuffer(AVFrame* frame, AVPixelFormat format, int width, int height)
{
    if (frame->buf[0] != nullptr && frame->format == format && frame->width == width && frame->height == height) {
        if (av_frame_make_writable(frame) < 0)
            throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
        return;
    }
    av_frame_unref(frame);
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
}

// Prova vera: l'encoder si apre e codifica un fotogramma.
bool works(const Candidate& candidate)
{
    if (avcodec_find_encoder_by_name(candidate.name) == nullptr)
        return false;
    try {
        VideoEncoderSettings s;
        s.codec = candidate.codec;
        s.width = 320;
        s.height = 240;
        CodecContextPtr ctx = openVideo(candidate, s);
        FramePtr frame(av_frame_alloc());
        frame->format = ctx->pix_fmt;
        frame->width = ctx->width;
        frame->height = ctx->height;
        if (av_frame_get_buffer(frame.get(), 0) < 0)
            return false;
        const std::ptrdiff_t linesizes[4] = {frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3]};
        av_image_fill_black(frame->data, linesizes, ctx->pix_fmt, AVCOL_RANGE_MPEG, ctx->width, ctx->height);
        frame->pts = 0;
        int packets = 0;
        const PacketSink count = [&](AVPacket*) { ++packets; };
        sendAndDrain(ctx.get(), frame.get(), count);
        sendAndDrain(ctx.get(), nullptr, count);
        return packets > 0;
    } catch (const MediaError&) {
        return false;
    }
}

const Candidate* findCandidate(VideoCodec codec, EncoderBackend backend)
{
    for (const Candidate& c : kCandidates) {
        if (c.codec == codec && c.backend == backend)
            return &c;
    }
    return nullptr;
}

}  // namespace

std::string codecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return "H.264";
    case VideoCodec::Hevc:
        return "HEVC";
    case VideoCodec::Av1:
        return "AV1";
    }
    return {};
}

std::string backendName(EncoderBackend backend)
{
    switch (backend) {
    case EncoderBackend::Nvenc:
        return "NVIDIA NVENC";
    case EncoderBackend::Qsv:
        return "Intel Quick Sync";
    case EncoderBackend::Amf:
        return "AMD AMF";
    case EncoderBackend::MediaFoundation:
        return "Media Foundation";
    }
    return {};
}

std::string describe(const VideoEncoderInfo& info)
{
    return codecName(info.codec) + " - " + backendName(info.backend) + " (" + info.ffmpegName + ")";
}

VideoEncoderTuning videoEncoderTuning(const VideoEncoderInfo& info)
{
    VideoEncoderTuning tuning;
    switch (info.backend) {
    case EncoderBackend::Nvenc:
        tuning.presets = kNvencPresets;
        tuning.defaultPreset = "p5";
        tuning.cqMax = info.codec == VideoCodec::Av1 ? 63 : 51;
        tuning.cqHigh = info.codec == VideoCodec::Av1 ? 28 : 19;
        tuning.cqMedium = info.codec == VideoCodec::Av1 ? 34 : 25;
        break;
    case EncoderBackend::Qsv:
        tuning.presets = kQsvPresets;
        tuning.defaultPreset = "medium";
        tuning.cqMin = 1;
        tuning.cqHigh = 20;
        tuning.cqMedium = 26;
        break;
    case EncoderBackend::Amf:
        tuning.presets = kAmfPresets;
        tuning.defaultPreset = "quality";
        tuning.cqHigh = 20;
        tuning.cqMedium = 26;
        break;
    case EncoderBackend::MediaFoundation:
        tuning.cqMin = 1;
        break;
    }
    return tuning;
}

namespace {

// Il formato di uscita (muxer) per l'estensione, o nullptr.
const AVOutputFormat* outputFormatFor(const std::string& extension)
{
    std::string name = "x";
    if (extension.empty() || extension.front() != '.')
        name += '.';
    name += extension;
    return av_guess_format(nullptr, name.c_str(), nullptr);
}

bool containerAccepts(AVCodecID id, const std::string& extension)
{
    const AVOutputFormat* format = outputFormatFor(extension);
    return format != nullptr && avformat_query_codec(format, id, FF_COMPLIANCE_NORMAL) == 1;
}

}  // namespace

bool videoCodecFitsContainer(VideoCodec codec, const std::string& extension)
{
    const AVCodecID id = codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : codec == VideoCodec::Av1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_H264;
    return containerAccepts(id, extension);
}

namespace {

// Gli encoder funzionanti di un codec. La prova si fa la prima volta che si chiede quel codec, anche da più thread
// insieme, e il risultato resta in memoria: l'esportazione prova solo i codec che usa (H.264, HEVC), AV1 solo quando
// serve l'elenco completo.
const std::vector<VideoEncoderInfo>& workingEncoders(VideoCodec codec)
{
    constexpr std::size_t kCodecs = 3;
    static std::vector<VideoEncoderInfo> encoders[kCodecs];
    static std::once_flag once[kCodecs];
    const auto index = static_cast<std::size_t>(codec);
    std::call_once(once[index], [codec, index] {
        // La prova si fa sempre in un thread a parte, anche se la chiede il thread della finestra: Media Foundation usa
        // COM, e nel thread della finestra (COM a thread singolo, quello delle finestre di Windows) non funziona e lascia
        // bloccata la finestra di Windows per salvare che si apre dopo.
        std::thread probe([codec, index] {
            // Gli encoder che non trovano la loro scheda video lo scrivono nel registro di FFmpeg: durante la prova no.
            const int level = av_log_get_level();
            av_log_set_level(AV_LOG_QUIET);
            for (const Candidate& c : kCandidates) {
                if (c.codec == codec && works(c))
                    encoders[index].push_back({c.codec, c.backend, c.name});
            }
            av_log_set_level(level);
        });
        probe.join();
    });
    return encoders[index];
}

}  // namespace

const std::vector<VideoEncoderInfo>& availableVideoEncoders()
{
    static std::vector<VideoEncoderInfo> encoders;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1}) {
            const std::vector<VideoEncoderInfo>& found = workingEncoders(codec);
            encoders.insert(encoders.end(), found.begin(), found.end());
        }
    });
    return encoders;
}

std::optional<VideoEncoderInfo> preferredVideoEncoder(VideoCodec codec)
{
    const std::vector<VideoEncoderInfo>& found = workingEncoders(codec);
    if (found.empty())
        return std::nullopt;
    return found.front();
}

// ---------------------------------------------------------------------------------------------------------------
// Video

struct VideoEncoder::Impl
{
    VideoEncoderInfo info;
    CodecContextPtr ctx;
    std::unique_ptr<SwsContext, SwsDeleter> sws;
    bool finished = false;

    // Tenuti fra un fotogramma e l'altro per non allocare ogni volta.
    PacketPtr packet{av_packet_alloc()};
    FramePtr referenced{av_frame_alloc()};  // fotogramma già nel formato dell'encoder: solo un riferimento
    FramePtr converted{av_frame_alloc()};   // fotogramma convertito alla misura e al formato dell'encoder
    FramePtr fitted{av_frame_alloc()};      // immagine ridimensionata, prima di metterla fra le bande nere
    // Con cui è preparato `sws`: formato, misura di ingresso e di uscita, gamma dei colori.
    struct SwsKey
    {
        int format = -1;
        int width = 0;
        int height = 0;
        int outWidth = 0;
        int outHeight = 0;
        bool fullRange = false;
        bool operator==(const SwsKey&) const = default;
    } swsKey;
};

VideoEncoder::VideoEncoder(const VideoEncoderSettings& settings)
    : impl_(std::make_unique<Impl>())
{
    if (settings.width <= 0 || settings.height <= 0)
        throw MediaError("Misure del video da codificare non valide.");
    if (!impl_->packet || !impl_->referenced || !impl_->converted || !impl_->fitted)
        throw MediaError("Memoria insufficiente per l'encoder video.");
    std::vector<VideoEncoderInfo> choices;
    for (const VideoEncoderInfo& info : workingEncoders(settings.codec)) {
        if (!settings.backend || info.backend == *settings.backend)
            choices.push_back(info);
    }
    if (choices.empty()) {
        throw MediaError("Su questo PC non c'è un encoder " + codecName(settings.codec)
                         + (settings.backend ? " " + backendName(*settings.backend) : std::string()) + " funzionante.");
    }
    std::string problems;
    for (const VideoEncoderInfo& info : choices) {
        try {
            impl_->ctx = openVideo(*findCandidate(info.codec, info.backend), settings);
            impl_->info = info;
            return;
        } catch (const MediaError& e) {
            problems += std::string("\n") + e.what();
        }
    }
    throw MediaError("Nessun encoder " + codecName(settings.codec) + " si apre con queste impostazioni:" + problems);
}

VideoEncoder::~VideoEncoder() = default;

const VideoEncoderInfo& VideoEncoder::info() const
{
    return impl_->info;
}

Rational VideoEncoder::timeBase() const
{
    return {impl_->ctx->time_base.num, impl_->ctx->time_base.den};
}

void VideoEncoder::copyParameters(AVCodecParameters* parameters) const
{
    avcodec_parameters_from_context(parameters, impl_->ctx.get());
}

void VideoEncoder::encode(const AVFrame* frame, bool forceKeyframe, const PacketSink& sink)
{
    if (impl_->finished)
        throw MediaError("L'encoder video è già stato chiuso.");
    AVCodecContext* ctx = impl_->ctx.get();

    // Fotogramma della scheda video: prima in memoria normale.
    FramePtr software;
    const AVFrame* source = frame;
    if (frame->hw_frames_ctx != nullptr) {
        software.reset(av_frame_alloc());
        const int ret = av_hwframe_transfer_data(software.get(), frame, 0);
        if (ret < 0)
            throw MediaError("Fotogramma della scheda video non leggibile: " + ffmpegError(ret));
        av_frame_copy_props(software.get(), frame);
        source = software.get();
    }

    AVFrame* work = nullptr;
    if (source->format == ctx->pix_fmt && source->width == ctx->width && source->height == ctx->height
        && source->color_range != AVCOL_RANGE_JPEG) {
        work = impl_->referenced.get();
        av_frame_unref(work);
        if (av_frame_ref(work, source) < 0)
            throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    } else {
        // Formato o misura diversi: conversione. Le proporzioni dell'immagine restano quelle dell'originale (con le
        // proporzioni dei suoi pixel): se sono diverse da quelle del video codificato, bande nere ai lati o sopra e sotto.
        work = impl_->converted.get();
        prepareBuffer(work, ctx->pix_fmt, ctx->width, ctx->height);
        const AVRational sar = source->sample_aspect_ratio.num > 0 ? source->sample_aspect_ratio : AVRational{1, 1};
        const double aspect = static_cast<double>(source->width) * sar.num / (static_cast<double>(source->height) * sar.den);
        int width = ctx->width;
        int height = ctx->height;
        if (aspect > static_cast<double>(ctx->width) / ctx->height + 1e-3)
            height = std::min(ctx->height, static_cast<int>(std::lround(ctx->width / aspect / 2.0)) * 2);
        else if (aspect < static_cast<double>(ctx->width) / ctx->height - 1e-3)
            width = std::min(ctx->width, static_cast<int>(std::lround(ctx->height * aspect / 2.0)) * 2);
        // I formati "J" (es. yuvj420p, dei video dei telefoni e di alcuni registratori) sono quelli normali a gamma piena:
        // la conversione va detta così, altrimenti i colori escono un po' sbiaditi o troppo contrastati.
        AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(source->format);
        bool fullRange = source->color_range == AVCOL_RANGE_JPEG;
        switch (sourceFormat) {
        case AV_PIX_FMT_YUVJ420P: sourceFormat = AV_PIX_FMT_YUV420P; fullRange = true; break;
        case AV_PIX_FMT_YUVJ422P: sourceFormat = AV_PIX_FMT_YUV422P; fullRange = true; break;
        case AV_PIX_FMT_YUVJ444P: sourceFormat = AV_PIX_FMT_YUV444P; fullRange = true; break;
        default: break;
        }
        impl_->sws.reset(sws_getCachedContext(impl_->sws.release(), source->width, source->height, sourceFormat, width, height,
                                              ctx->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
        if (!impl_->sws)
            throw MediaError("Conversione del fotogramma da codificare non possibile.");
        // I colori si dicono al convertitore solo quando cambia qualcosa (di solito mai dopo il primo fotogramma).
        const Impl::SwsKey key{sourceFormat, source->width, source->height, width, height, fullRange};
        if (!(key == impl_->swsKey)) {
            const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
            sws_setColorspaceDetails(impl_->sws.get(), coefficients, fullRange ? 1 : 0, coefficients, 0, 0, 1 << 16, 1 << 16);
            impl_->swsKey = key;
        }
        if (width == ctx->width && height == ctx->height) {
            sws_scale(impl_->sws.get(), source->data, source->linesize, 0, source->height, work->data, work->linesize);
        } else {
            AVFrame* fitted = impl_->fitted.get();
            prepareBuffer(fitted, ctx->pix_fmt, width, height);
            sws_scale(impl_->sws.get(), source->data, source->linesize, 0, source->height, fitted->data, fitted->linesize);
            // Nero dappertutto, poi l'immagine al centro (in posizioni pari, per il colore sottocampionato).
            const std::ptrdiff_t linesizes[4] = {work->linesize[0], work->linesize[1], work->linesize[2], work->linesize[3]};
            av_image_fill_black(work->data, linesizes, ctx->pix_fmt, AVCOL_RANGE_MPEG, ctx->width, ctx->height);
            const int x = (ctx->width - width) / 2 & ~1;
            const int y = (ctx->height - height) / 2 & ~1;
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(ctx->pix_fmt);
            for (int plane = 0; plane < av_pix_fmt_count_planes(ctx->pix_fmt); ++plane) {
                const bool chroma = plane == 1 || plane == 2;
                const int rows = chroma ? AV_CEIL_RSHIFT(height, desc->log2_chroma_h) : height;
                const int top = chroma ? y >> desc->log2_chroma_h : y;
                const int left = av_image_get_linesize(ctx->pix_fmt, x, plane);
                const int bytes = av_image_get_linesize(ctx->pix_fmt, width, plane);
                av_image_copy_plane(work->data[plane] + static_cast<std::ptrdiff_t>(top) * work->linesize[plane] + left,
                                    work->linesize[plane], fitted->data[plane], fitted->linesize[plane], bytes, rows);
            }
        }
        av_frame_copy_props(work, source);
        work->sample_aspect_ratio = {1, 1};
    }
    work->pts = frame->pts;
    work->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (forceKeyframe)
        work->flags |= AV_FRAME_FLAG_KEY;
    else
        work->flags &= ~AV_FRAME_FLAG_KEY;
    sendAndDrain(ctx, work, sink, impl_->packet.get());
    av_frame_unref(impl_->referenced.get());  // non tiene in vita il fotogramma dell'originale fino al prossimo
}

void VideoEncoder::finish(const PacketSink& sink)
{
    if (impl_->finished)
        return;
    impl_->finished = true;
    sendAndDrain(impl_->ctx.get(), nullptr, sink, impl_->packet.get());
}

// ---------------------------------------------------------------------------------------------------------------
// Audio

namespace {

struct AudioEncoderChoice
{
    const AVCodec* codec = nullptr;
    bool experimental = false;  // va usato con strict_std_compliance sperimentale (es. l'Opus interno di FFmpeg)
};

// L'encoder di FFmpeg per il codec: prima quello di una libreria esterna (migliore), poi quello interno.
AudioEncoderChoice findAudioEncoder(AudioCodec codec)
{
    const auto byName = [](const char* name, bool experimental) {
        AudioEncoderChoice choice;
        choice.codec = avcodec_find_encoder_by_name(name);
        choice.experimental = experimental && choice.codec != nullptr;
        return choice;
    };
    AudioEncoderChoice choice;
    switch (codec) {
    case AudioCodec::Aac:
        return byName("aac", false);
    case AudioCodec::Mp3:
        return byName("libmp3lame", false);
    case AudioCodec::Opus:
        choice = byName("libopus", false);
        return choice.codec != nullptr ? choice : byName("opus", true);
    case AudioCodec::Ac3:
        return byName("ac3", false);
    case AudioCodec::Flac:
        return byName("flac", false);
    }
    return choice;
}

AVCodecID codecIdOf(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Aac: return AV_CODEC_ID_AAC;
    case AudioCodec::Mp3: return AV_CODEC_ID_MP3;
    case AudioCodec::Opus: return AV_CODEC_ID_OPUS;
    case AudioCodec::Ac3: return AV_CODEC_ID_AC3;
    case AudioCodec::Flac: return AV_CODEC_ID_FLAC;
    }
    return AV_CODEC_ID_NONE;
}

// Il formato dei campioni preferito fra quelli ammessi dall'encoder (prima i float, poi gli interi).
AVSampleFormat chooseSampleFormat(const AVCodec* codec)
{
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) < 0 || configs == nullptr)
        return AV_SAMPLE_FMT_FLTP;
    const auto* formats = static_cast<const AVSampleFormat*>(configs);
    for (const AVSampleFormat wanted : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16,
                                        AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32}) {
        if (std::find(formats, formats + count, wanted) != formats + count)
            return wanted;
    }
    return count > 0 ? formats[0] : AV_SAMPLE_FMT_FLTP;
}

// La frequenza chiesta se l'encoder la ammette, altrimenti la più vicina fra quelle ammesse.
int chooseSampleRate(const AVCodec* codec, int wanted)
{
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &count) < 0 || configs == nullptr
        || count <= 0)
        return wanted;
    const auto* rates = static_cast<const int*>(configs);
    int best = rates[0];
    for (int i = 0; i < count; ++i) {
        if (rates[i] == wanted)
            return wanted;
        if (std::abs(rates[i] - wanted) < std::abs(best - wanted))
            best = rates[i];
    }
    return best;
}

}  // namespace

std::string audioCodecName(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Aac: return "AAC";
    case AudioCodec::Mp3: return "MP3";
    case AudioCodec::Opus: return "Opus";
    case AudioCodec::Ac3: return "AC-3";
    case AudioCodec::Flac: return "FLAC";
    }
    return {};
}

const std::vector<AudioCodec>& availableAudioCodecs()
{
    static const std::vector<AudioCodec> codecs = [] {
        std::vector<AudioCodec> found;
        for (const AudioCodec codec : {AudioCodec::Aac, AudioCodec::Mp3, AudioCodec::Opus, AudioCodec::Ac3, AudioCodec::Flac}) {
            if (findAudioEncoder(codec).codec != nullptr)
                found.push_back(codec);
        }
        return found;
    }();
    return codecs;
}

bool audioCodecFitsContainer(AudioCodec codec, const std::string& extension)
{
    return containerAccepts(codecIdOf(codec), extension);
}

std::vector<int> audioBitRatesKbps(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Aac: return {96, 128, 160, 192, 256, 320};
    case AudioCodec::Mp3: return {96, 128, 160, 192, 256, 320};
    case AudioCodec::Opus: return {64, 96, 128, 160, 192, 256};
    case AudioCodec::Ac3: return {192, 256, 384, 448, 640};
    case AudioCodec::Flac: return {};
    }
    return {};
}

int defaultAudioBitRateKbps(AudioCodec codec)
{
    return codec == AudioCodec::Ac3 ? 384 : 192;
}

struct AudioEncoder::Impl
{
    CodecContextPtr ctx;
    std::unique_ptr<SwrContext, SwrDeleter> swr;
    std::unique_ptr<AVAudioFifo, FifoDeleter> fifo;
    // Formato dell'audio in ingresso per cui swr è preparato.
    int inRate = 0;
    int inFormat = -1;
    AVChannelLayout inLayout{};
    std::int64_t nextPts = 0;  // campioni già mandati all'encoder
    bool finished = false;

    // Tenuti fra una chiamata e l'altra per non allocare ogni volta.
    PacketPtr packet{av_packet_alloc()};
    FramePtr block{av_frame_alloc()};      // il blocco di frame_size campioni che va all'encoder
    FramePtr converted{av_frame_alloc()};  // l'audio in ingresso convertito, prima di metterlo in coda
    int convertedCapacity = 0;             // campioni che stanno in `converted`

    ~Impl() { av_channel_layout_uninit(&inLayout); }

    // Manda all'encoder blocchi di frame_size campioni dalla coda (anche l'ultimo, più corto, se `all`).
    void sendQueued(bool all, const PacketSink& sink)
    {
        const int size = ctx->frame_size > 0 ? ctx->frame_size : 1024;
        while (av_audio_fifo_size(fifo.get()) >= size || (all && av_audio_fifo_size(fifo.get()) > 0)) {
            const int n = std::min(size, av_audio_fifo_size(fifo.get()));
            AVFrame* frame = block.get();
            frame->nb_samples = size;
            if (frame->buf[0] != nullptr) {
                if (av_frame_make_writable(frame) < 0)
                    throw MediaError("Memoria insufficiente per l'audio da codificare.");
            } else {
                frame->format = ctx->sample_fmt;
                frame->sample_rate = ctx->sample_rate;
                if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0 || av_frame_get_buffer(frame, 0) < 0)
                    throw MediaError("Memoria insufficiente per l'audio da codificare.");
            }
            frame->nb_samples = n;  // l'ultimo blocco può essere più corto: il buffer resta quello intero
            av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(frame->data), n);
            frame->pts = nextPts;
            nextPts += n;
            sendAndDrain(ctx.get(), frame, sink, packet.get());
        }
    }
};

AudioEncoder::AudioEncoder(const AudioEncoderSettings& settings)
    : impl_(std::make_unique<Impl>())
{
    const AudioEncoderChoice choice = findAudioEncoder(settings.codec);
    if (choice.codec == nullptr)
        throw MediaError("Encoder " + audioCodecName(settings.codec) + " non presente in questa versione di FFmpeg.");
    const std::string name = audioCodecName(settings.codec);
    impl_->ctx.reset(avcodec_alloc_context3(choice.codec));
    if (!impl_->ctx || !impl_->packet || !impl_->block || !impl_->converted)
        throw MediaError("Memoria insufficiente per l'encoder " + name + ".");
    AVCodecContext* ctx = impl_->ctx.get();
    ctx->sample_rate = chooseSampleRate(choice.codec, settings.sampleRate);
    ctx->sample_fmt = chooseSampleFormat(choice.codec);
    av_channel_layout_default(&ctx->ch_layout, std::max(1, settings.channels));
    if (settings.codec != AudioCodec::Flac)  // FLAC è senza perdita: il bitrate non c'entra
        ctx->bit_rate = settings.bitRate > 0 ? settings.bitRate : defaultAudioBitRateKbps(settings.codec) * 1000;
    ctx->time_base = {1, ctx->sample_rate};
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // dati extra nel contenitore (MP4, MKV)
    if (choice.experimental)
        ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (const int ret = avcodec_open2(ctx, choice.codec, nullptr); ret < 0)
        throw MediaError("L'encoder " + name + " non si apre: " + ffmpegError(ret));
    impl_->fifo.reset(av_audio_fifo_alloc(ctx->sample_fmt, ctx->ch_layout.nb_channels, std::max(ctx->frame_size, 1024) * 4));
    if (!impl_->fifo)
        throw MediaError("Memoria insufficiente per l'encoder " + name + ".");
}

AudioEncoder::~AudioEncoder() = default;

Rational AudioEncoder::timeBase() const
{
    return {impl_->ctx->time_base.num, impl_->ctx->time_base.den};
}

void AudioEncoder::copyParameters(AVCodecParameters* parameters) const
{
    avcodec_parameters_from_context(parameters, impl_->ctx.get());
}

void AudioEncoder::encode(const AVFrame* frame, const PacketSink& sink)
{
    if (impl_->finished)
        throw MediaError("L'encoder audio è già stato chiuso.");
    AVCodecContext* ctx = impl_->ctx.get();
    // Il convertitore si prepara (di nuovo) quando cambia il formato dell'audio in ingresso.
    if (!impl_->swr || frame->sample_rate != impl_->inRate || frame->format != impl_->inFormat
        || av_channel_layout_compare(&frame->ch_layout, &impl_->inLayout) != 0) {
        SwrContext* swr = nullptr;
        if (swr_alloc_set_opts2(&swr, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, &frame->ch_layout,
                                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0
            || swr_init(swr) < 0) {
            swr_free(&swr);
            throw MediaError("Conversione dell'audio da codificare non possibile.");
        }
        impl_->swr.reset(swr);
        impl_->inRate = frame->sample_rate;
        impl_->inFormat = frame->format;
        av_channel_layout_uninit(&impl_->inLayout);
        av_channel_layout_copy(&impl_->inLayout, &frame->ch_layout);
    }
    const int capacity = std::max(swr_get_out_samples(impl_->swr.get(), frame->nb_samples), 1);
    AVFrame* converted = impl_->converted.get();
    if (capacity > impl_->convertedCapacity) {  // si rialloca solo se serve più spazio di prima
        av_frame_unref(converted);
        converted->nb_samples = capacity;
        converted->format = ctx->sample_fmt;
        converted->sample_rate = ctx->sample_rate;
        if (av_channel_layout_copy(&converted->ch_layout, &ctx->ch_layout) < 0 || av_frame_get_buffer(converted, 0) < 0)
            throw MediaError("Memoria insufficiente per l'audio da codificare.");
        impl_->convertedCapacity = capacity;
    }
    const int got = swr_convert(impl_->swr.get(), converted->data, impl_->convertedCapacity,
                                const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
    if (got < 0)
        throw MediaError("Conversione dell'audio da codificare non riuscita.");
    av_audio_fifo_write(impl_->fifo.get(), reinterpret_cast<void**>(converted->data), got);
    impl_->sendQueued(false, sink);
}

void AudioEncoder::finish(const PacketSink& sink)
{
    if (impl_->finished)
        return;
    impl_->finished = true;
    AVCodecContext* ctx = impl_->ctx.get();
    // Quello che è rimasto nel convertitore, poi l'ultimo blocco (più corto) e la fine.
    if (impl_->swr) {
        FramePtr rest(av_frame_alloc());
        rest->nb_samples = 4096;
        rest->format = ctx->sample_fmt;
        rest->sample_rate = ctx->sample_rate;
        av_channel_layout_copy(&rest->ch_layout, &ctx->ch_layout);
        if (av_frame_get_buffer(rest.get(), 0) >= 0) {
            const int got = swr_convert(impl_->swr.get(), rest->data, rest->nb_samples, nullptr, 0);
            if (got > 0)
                av_audio_fifo_write(impl_->fifo.get(), reinterpret_cast<void**>(rest->data), got);
        }
    }
    impl_->sendQueued(true, sink);
    sendAndDrain(ctx, nullptr, sink, impl_->packet.get());
}

}  // namespace editor::core
