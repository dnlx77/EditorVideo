#include "EditorCore/FfmpegUtil.h"

#include "EditorCore/MediaProbe.h"

#include <algorithm>
#include <filesystem>

namespace editor::core::detail {

namespace {

std::filesystem::path pathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

bool isCancelled(const std::atomic<bool>* cancel)
{
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

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

}  // namespace editor::core::detail
