#include "EditorCore/FfmpegUtil.h"

#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace editor::core::detail {

bool isCancelled(const std::atomic<bool>* cancel)
{
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

namespace {

int interruptCallback(void* opaque)
{
    return isCancelled(static_cast<const std::atomic<bool>*>(opaque)) ? 1 : 0;
}

}  // namespace

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

FormatContextPtr openInput(const std::string& utf8Path, const std::atomic<bool>* cancel)
{
    std::error_code ec;
    const std::filesystem::path path = pathFromUtf8(utf8Path);
    if (!std::filesystem::exists(path, ec))
        throw ProbeError("Il file non esiste: " + utf8Path);
    if (!std::filesystem::is_regular_file(path, ec))
        throw ProbeError("Il percorso non è un file: " + utf8Path);

    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr)
        throw ProbeError("Memoria insufficiente per aprire il file.");
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

    return fmt;
}

double startOffsetSeconds(const AVFormatContext* fmt)
{
    return fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / double(AV_TIME_BASE) : 0.0;
}

int findMainStream(const AVFormatContext* fmt, AVMediaType type)
{
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == type && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

void keepOnlyStream(AVFormatContext* fmt, int index)
{
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        fmt->streams[i]->discard = static_cast<int>(i) == index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

OpenedStream openMainStream(const std::string& utf8Path, AVMediaType type)
{
    const std::string kind = type == AVMEDIA_TYPE_VIDEO ? "video" : "audio";
    OpenedStream opened;
    opened.fmt = openInput(utf8Path);
    opened.index = findMainStream(opened.fmt.get(), type);
    if (opened.index < 0)
        throw MediaError("Il file non contiene un flusso " + kind + ": " + utf8Path);
    opened.stream = opened.fmt->streams[opened.index];
    keepOnlyStream(opened.fmt.get(), opened.index);

    opened.codec = avcodec_find_decoder(opened.stream->codecpar->codec_id);
    if (opened.codec == nullptr)
        throw MediaError("Nessun decoder disponibile per il codec " + kind + " "
                         + std::string(avcodec_get_name(opened.stream->codecpar->codec_id)) + ".");
    opened.timeBase = av_q2d(opened.stream->time_base);
    opened.startOffset = startOffsetSeconds(opened.fmt.get());
    return opened;
}

void seekStream(AVFormatContext* fmt, int index, double seconds, double startOffset)
{
    const double timeBase = av_q2d(fmt->streams[index]->time_base);
    const auto ts = static_cast<std::int64_t>(std::llround((seconds + startOffset) / timeBase));
    if (av_seek_frame(fmt, index, ts, AVSEEK_FLAG_BACKWARD) < 0)
        avformat_seek_file(fmt, index, INT64_MIN, ts, ts, 0);
}

double durationOf(const AVFormatContext* fmt, const AVStream* stream)
{
    if (fmt->duration != AV_NOPTS_VALUE)
        return fmt->duration / double(AV_TIME_BASE);
    if (stream->duration != AV_NOPTS_VALUE)
        return stream->duration * av_q2d(stream->time_base);
    return -1.0;
}

std::string describeChannelLayout(const AVCodecParameters* par)
{
    AVChannelLayout layout = {};
    defaultOrCopyLayout(&layout, par->ch_layout);
    char text[128] = {};
    std::string result;
    if (av_channel_layout_describe(&layout, text, sizeof(text)) > 0)
        result = text;
    av_channel_layout_uninit(&layout);
    return result;
}

}  // namespace editor::core::detail
