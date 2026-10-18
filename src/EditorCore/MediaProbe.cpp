#include "EditorCore/MediaProbe.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace editor::core {

namespace {

struct FormatContextDeleter
{
    void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

struct PacketDeleter
{
    void operator()(AVPacket* pkt) const { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::filesystem::path pathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

bool isCancelled(const std::atomic<bool>* cancel)
{
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

std::string dictValue(const AVDictionary* dict, const char* key, int flags = 0)
{
    const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, flags);
    return entry != nullptr && entry->value != nullptr ? entry->value : std::string();
}

StreamKind kindOf(AVMediaType type)
{
    switch (type) {
    case AVMEDIA_TYPE_VIDEO: return StreamKind::Video;
    case AVMEDIA_TYPE_AUDIO: return StreamKind::Audio;
    case AVMEDIA_TYPE_SUBTITLE: return StreamKind::Subtitle;
    default: return StreamKind::Other;
    }
}

StreamInfo describeStream(const AVFormatContext* fmt, AVStream* st)
{
    const AVCodecParameters* par = st->codecpar;

    StreamInfo info;
    info.index = st->index;
    info.kind = kindOf(par->codec_type);
    info.codec = avcodec_get_name(par->codec_id);
    if (const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id); desc != nullptr && desc->long_name)
        info.codecLongName = desc->long_name;
    if (const char* profile = avcodec_profile_name(par->codec_id, par->profile))
        info.profile = profile;
    info.language = dictValue(st->metadata, "language");

    info.bitRate = par->bit_rate;
    if (info.bitRate <= 0) {
        // Matroska non ha un campo bitrate: mkvmerge lo scrive nel tag "BPS" (o "BPS-eng").
        const std::string bps = dictValue(st->metadata, "BPS", AV_DICT_IGNORE_SUFFIX);
        info.bitRate = bps.empty() ? 0 : std::strtoll(bps.c_str(), nullptr, 10);
    }

    if (st->duration != AV_NOPTS_VALUE)
        info.durationSeconds = st->duration * av_q2d(st->time_base);

    switch (info.kind) {
    case StreamKind::Video: {
        info.width = par->width;
        info.height = par->height;
        info.attachedPicture = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        const AVRational rate = av_guess_frame_rate(const_cast<AVFormatContext*>(fmt), st, nullptr);
        info.frameRate = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
        if (const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format)))
            info.pixelFormat = name;
        break;
    }
    case StreamKind::Audio: {
        info.sampleRate = par->sample_rate;
        info.channels = par->ch_layout.nb_channels;
        // Alcuni contenitori (es. PCM in Matroska) non salvano la disposizione dei canali:
        // in quel caso mostriamo quella standard per quel numero di canali ("stereo" invece di "2 channels").
        AVChannelLayout layout = {};
        if (par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&layout, par->ch_layout.nb_channels);
        else
            av_channel_layout_copy(&layout, &par->ch_layout);
        char text[128] = {};
        if (av_channel_layout_describe(&layout, text, sizeof(text)) > 0)
            info.channelLayout = text;
        av_channel_layout_uninit(&layout);
        break;
    }
    default:
        break;
    }
    return info;
}

// Primo flusso video vero (non una copertina): è quello su cui si faranno i tagli.
int mainVideoStream(const AVFormatContext* fmt)
{
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

std::vector<double> readKeyframes(AVFormatContext* fmt, int streamIndex, const std::atomic<bool>* cancel)
{
    // Leggiamo solo i pacchetti del flusso video: gli altri flussi vengono scartati dal demuxer.
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        fmt->streams[i]->discard = static_cast<int>(i) == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

    const AVStream* st = fmt->streams[streamIndex];
    const double timeBase = av_q2d(st->time_base);
    const double startOffset = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / double(AV_TIME_BASE) : 0.0;

    PacketPtr packet(av_packet_alloc());
    if (!packet)
        throw ProbeError("Memoria insufficiente per analizzare il file.");

    std::vector<double> times;
    for (;;) {
        if (isCancelled(cancel))
            throw ProbeCancelled();

        const int ret = av_read_frame(fmt, packet.get());
        if (ret < 0) {
            if (ret == AVERROR_EXIT || isCancelled(cancel))
                throw ProbeCancelled();
            break;  // fine del file (o coda danneggiata: teniamo quello che abbiamo letto)
        }

        if (packet->stream_index == streamIndex && (packet->flags & AV_PKT_FLAG_KEY) != 0) {
            const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (ts != AV_NOPTS_VALUE)
                times.push_back(ts * timeBase - startOffset);
        }
        av_packet_unref(packet.get());
    }

    std::sort(times.begin(), times.end());
    return times;
}

int interruptCallback(void* opaque)
{
    return isCancelled(static_cast<const std::atomic<bool>*>(opaque)) ? 1 : 0;
}

}  // namespace

ProbeCancelled::ProbeCancelled()
    : ProbeError("Analisi annullata.")
{
}

MediaInfo probeFile(const std::string& utf8Path, const std::atomic<bool>* cancel)
{
    std::error_code ec;
    const std::filesystem::path path = pathFromUtf8(utf8Path);
    if (!std::filesystem::exists(path, ec))
        throw ProbeError("Il file non esiste: " + utf8Path);
    if (!std::filesystem::is_regular_file(path, ec))
        throw ProbeError("Il percorso non è un file: " + utf8Path);

    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr)
        throw ProbeError("Memoria insufficiente per analizzare il file.");
    if (cancel != nullptr) {
        raw->interrupt_callback.callback = &interruptCallback;
        raw->interrupt_callback.opaque = const_cast<std::atomic<bool>*>(cancel);
    }

    const auto notMedia = [&](int ret) {
        return ProbeError("Il file non è un video o un audio riconosciuto (" + ffmpegError(ret) + "): " + utf8Path);
    };

    // FFmpeg su Windows accetta i percorsi in UTF-8, quindi funzionano anche lettere accentate e simili.
    // In caso di errore avformat_open_input libera il contesto da sola.
    if (const int ret = avformat_open_input(&raw, utf8Path.c_str(), nullptr, nullptr); ret < 0) {
        if (ret == AVERROR_EXIT || isCancelled(cancel))
            throw ProbeCancelled();
        throw notMedia(ret);
    }
    FormatContextPtr fmt(raw);

    // Un file qualsiasi (es. testo) a volte viene "riconosciuto" con punteggio bassissimo
    // e fallisce solo qui: per l'utente è comunque un file non riconosciuto.
    if (const int ret = avformat_find_stream_info(fmt.get(), nullptr); ret < 0) {
        if (ret == AVERROR_EXIT || isCancelled(cancel))
            throw ProbeCancelled();
        throw notMedia(ret);
    }

    const bool hasAudioOrVideo = std::any_of(fmt->streams, fmt->streams + fmt->nb_streams, [](const AVStream* st) {
        return st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
    });
    if (!hasAudioOrVideo)
        throw ProbeError("Il file non contiene flussi video o audio: " + utf8Path);

    MediaInfo info;
    info.path = utf8Path;
    info.formatName = fmt->iformat->name ? fmt->iformat->name : "";
    info.formatLongName = fmt->iformat->long_name ? fmt->iformat->long_name : "";
    if (fmt->duration != AV_NOPTS_VALUE)
        info.durationSeconds = fmt->duration / double(AV_TIME_BASE);
    info.bitRate = fmt->bit_rate;

    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        info.streams.push_back(describeStream(fmt.get(), fmt->streams[i]));

    info.keyframeStreamIndex = mainVideoStream(fmt.get());
    if (info.keyframeStreamIndex >= 0)
        info.keyframeTimes = readKeyframes(fmt.get(), info.keyframeStreamIndex, cancel);

    return info;
}

const char* streamKindName(StreamKind kind)
{
    switch (kind) {
    case StreamKind::Video: return "Video";
    case StreamKind::Audio: return "Audio";
    case StreamKind::Subtitle: return "Sottotitoli";
    default: return "Altro";
    }
}

std::string formatDuration(double seconds)
{
    if (seconds < 0 || !std::isfinite(seconds))
        return "sconosciuta";

    const long long totalMs = std::llround(seconds * 1000.0);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld", totalMs / 3'600'000,
                  totalMs / 60'000 % 60, totalMs / 1000 % 60, totalMs % 1000);
    return buffer;
}

std::string formatBitRate(std::int64_t bitsPerSecond)
{
    if (bitsPerSecond <= 0)
        return "sconosciuto";

    char buffer[32];
    if (bitsPerSecond < 1'000'000) {
        std::snprintf(buffer, sizeof(buffer), "%lld kb/s", static_cast<long long>(std::llround(bitsPerSecond / 1000.0)));
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.1f Mb/s", bitsPerSecond / 1'000'000.0);
    std::string text = buffer;
    std::replace(text.begin(), text.end(), '.', ',');  // virgola decimale all'italiana
    return text;
}

std::string formatFrameRate(double fps)
{
    if (fps <= 0 || !std::isfinite(fps))
        return "sconosciuto";

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", fps);
    std::string text = buffer;
    // Togliamo gli zeri finali: 25.000 -> 25, 29.970 -> 29,97, 23.976 resta 23,976.
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.')
        text.pop_back();
    std::replace(text.begin(), text.end(), '.', ',');
    return text + " fps";
}

}  // namespace editor::core
