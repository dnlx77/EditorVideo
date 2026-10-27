#include "EditorCore/MediaProbe.h"

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/KeyframeCache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace editor::core {

using namespace detail;

namespace {

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
    readStreamFormat(fmt, st, info);
    if (const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id); desc != nullptr && desc->long_name)
        info.codecLongName = desc->long_name;
    info.language = dictValue(st->metadata, "language");

    info.bitRate = par->bit_rate;
    if (info.bitRate <= 0) {
        // Matroska non ha un campo bitrate: mkvmerge lo scrive nel tag "BPS" (o "BPS-eng").
        const std::string bps = dictValue(st->metadata, "BPS", AV_DICT_IGNORE_SUFFIX);
        info.bitRate = bps.empty() ? 0 : std::strtoll(bps.c_str(), nullptr, 10);
    }

    if (st->duration != AV_NOPTS_VALUE)
        info.durationSeconds = st->duration * av_q2d(st->time_base);

    if (info.kind == StreamKind::Video)
        info.attachedPicture = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
    return info;
}

std::vector<double> readKeyframes(AVFormatContext* fmt, int streamIndex, const std::atomic<bool>* cancel)
{
    // Leggiamo solo i pacchetti del flusso video: gli altri flussi vengono scartati dal demuxer.
    keepOnlyStream(fmt, streamIndex);

    const AVStream* st = fmt->streams[streamIndex];
    const double timeBase = av_q2d(st->time_base);
    const double startOffset = startOffsetSeconds(fmt);

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
            const std::int64_t ts = packetTimestamp(packet.get());
            if (ts != AV_NOPTS_VALUE)
                times.push_back(ts * timeBase - startOffset);
        }
        av_packet_unref(packet.get());
    }

    std::sort(times.begin(), times.end());
    return times;
}

}  // namespace

ProbeCancelled::ProbeCancelled()
    : ProbeError("Analisi annullata.")
{
}

MediaInfo probeFile(const std::string& utf8Path, const std::atomic<bool>* cancel, const std::string& keyframeCacheFolder)
{
    FormatContextPtr fmt = openInput(utf8Path, cancel);

    MediaInfo info;
    info.path = utf8Path;
    info.formatName = fmt->iformat->name ? fmt->iformat->name : "";
    info.formatLongName = fmt->iformat->long_name ? fmt->iformat->long_name : "";
    if (fmt->duration != AV_NOPTS_VALUE)
        info.durationSeconds = fmt->duration / double(AV_TIME_BASE);
    info.bitRate = fmt->bit_rate;

    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        info.streams.push_back(describeStream(fmt.get(), fmt->streams[i]));

    info.keyframeStreamIndex = findMainStream(fmt.get(), AVMEDIA_TYPE_VIDEO);
    if (info.keyframeStreamIndex >= 0) {
        // Se i fotogrammi chiave di questo file (così com'è ora) sono già stati salvati, non serve scorrerlo tutto.
        int cachedStream = -1;
        std::vector<double> cachedTimes;
        if (loadKeyframeCache(keyframeCacheFolder, utf8Path, cachedStream, cachedTimes) &&
            cachedStream == info.keyframeStreamIndex) {
            info.keyframeTimes = std::move(cachedTimes);
        } else {
            info.keyframeTimes = readKeyframes(fmt.get(), info.keyframeStreamIndex, cancel);
            saveKeyframeCache(keyframeCacheFolder, utf8Path, info.keyframeStreamIndex, info.keyframeTimes);
        }
    }

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
