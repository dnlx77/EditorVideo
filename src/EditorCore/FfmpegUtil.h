#pragma once

// Funzioni di servizio interne a EditorCore per lavorare con FFmpeg. Non fanno parte dell'interfaccia pubblica.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
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

// Disposizione dei canali in testo, es. "stereo", "5.1(side)". Alcuni contenitori (es. PCM in Matroska) non la
// salvano: in quel caso si usa quella standard per quel numero di canali ("stereo" invece di "2 channels").
std::string describeChannelLayout(const AVCodecParameters* par);

// Riempie i campi comuni a StreamInfo (analisi del file) e StreamSignature (controllo di compatibilità): codec,
// profilo e, secondo il tipo di flusso, dimensioni, formato dei pixel e fotogrammi al secondo oppure frequenza,
// numero e disposizione dei canali.
template <typename Stream>
void readStreamFormat(const AVFormatContext* fmt, const AVStream* st, Stream& out)
{
    const AVCodecParameters* par = st->codecpar;
    out.codec = avcodec_get_name(par->codec_id);
    if (const char* profile = avcodec_profile_name(par->codec_id, par->profile))
        out.profile = profile;
    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        out.width = par->width;
        out.height = par->height;
        if (const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format)))
            out.pixelFormat = name;
        const AVRational rate =
            av_guess_frame_rate(const_cast<AVFormatContext*>(fmt), const_cast<AVStream*>(st), nullptr);
        out.frameRate = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        out.sampleRate = par->sample_rate;
        out.channels = par->ch_layout.nb_channels;
        out.channelLayout = describeChannelLayout(par);
    }
}

}  // namespace editor::core::detail
