#pragma once

// Funzioni di servizio interne a EditorCore per lavorare con FFmpeg. Non fanno parte dell'interfaccia pubblica.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <memory>
#include <string>

namespace editor::core::detail {

struct FormatContextDeleter
{
    void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

struct CodecContextDeleter
{
    void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

struct PacketDeleter
{
    void operator()(AVPacket* pkt) const { av_packet_free(&pkt); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct FrameDeleter
{
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

std::string ffmpegError(int code);

// true se `cancel` c'è ed è diventato true (richiesta di annullare un'operazione lunga).
bool isCancelled(const std::atomic<bool>* cancel);

// Apre il file (percorso UTF-8) e legge le informazioni sui flussi.
// Lancia ProbeCancelled se `cancel` diventa true, ProbeError con un messaggio in italiano negli altri casi.
FormatContextPtr openInput(const std::string& utf8Path, const std::atomic<bool>* cancel = nullptr);

// Inizio del file in secondi: i tempi mostrati all'utente partono da qui (0 = inizio del file).
double startOffsetSeconds(const AVFormatContext* fmt);

// Primo flusso del tipo richiesto, escluse le copertine (immagini allegate). -1 se non c'è.
int findMainStream(const AVFormatContext* fmt, AVMediaType type);

}  // namespace editor::core::detail
