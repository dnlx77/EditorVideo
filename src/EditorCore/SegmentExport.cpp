#include "EditorCore/SegmentExport.h"

#include "EditorCore/CutSegment.h"
#include "EditorCore/FfmpegUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace editor::core {

using namespace detail;

namespace {

constexpr double kEpsilon = 0.001;
// Dopo OUT si legge ancora un po': in un file i pacchetti dei vari flussi sono mescolati e, con i B-frame,
// un fotogramma prima di OUT può arrivare dopo uno successivo.
constexpr double kReadPastOut = 1.0;

struct OutputDeleter
{
    void operator()(AVFormatContext* ctx) const
    {
        if (ctx->pb != nullptr && (ctx->oformat->flags & AVFMT_NOFILE) == 0)
            avio_closep(&ctx->pb);
        avformat_free_context(ctx);
    }
};
using OutputPtr = std::unique_ptr<AVFormatContext, OutputDeleter>;

struct StreamCopy
{
    int output = -1;  // indice del flusso nel file di uscita, -1 = non copiato
    bool video = false;
    bool started = false;  // video: trovato il fotogramma chiave di partenza
    bool finished = false;
    std::int64_t offset = 0;  // IN nella base dei tempi del flusso: si sottrae a tutti i tempi
    std::vector<PacketPtr> pending;  // video oltre OUT tenuto da parte (può servire come riferimento)
};

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool samePath(const std::string& a, const std::string& b)
{
    std::error_code ec;
    const std::filesystem::path pa = pathFromUtf8(a);
    const std::filesystem::path pb = pathFromUtf8(b);
    if (std::filesystem::exists(pb, ec) && std::filesystem::equivalent(pa, pb, ec))
        return true;
    // Su Windows i nomi dei file non distinguono maiuscole e minuscole.
    const auto ca = std::filesystem::weakly_canonical(pa, ec).u8string();
    const auto cb = std::filesystem::weakly_canonical(pb, ec).u8string();
    return lowercase(std::string(ca.begin(), ca.end())) == lowercase(std::string(cb.begin(), cb.end()));
}

// Codec tag dell'originale se il contenitore di uscita lo accetta, altrimenti 0 (lo sceglie il muxer).
unsigned codecTagFor(const AVOutputFormat* format, const AVCodecParameters* par)
{
    if (format->codec_tag == nullptr)
        return par->codec_tag;
    if (par->codec_tag != 0 && av_codec_get_id(format->codec_tag, par->codec_tag) == par->codec_id)
        return par->codec_tag;
    return 0;
}

}  // namespace

ExportCancelled::ExportCancelled()
    : MediaError("Esportazione annullata.")
{
}

ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, double inSeconds,
                           double outSeconds, const ExportProgress& progress)
{
    if (!(outSeconds > inSeconds + kEpsilon))
        throw MediaError("OUT deve stare dopo IN.");
    if (samePath(inputPath, outputPath))
        throw MediaError("Il file di destinazione deve essere diverso dall'originale.");

    FormatContextPtr input = openInput(inputPath);
    const double start = startOffsetSeconds(input.get());

    AVFormatContext* rawOutput = nullptr;
    if (avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, outputPath.c_str()) < 0 || rawOutput == nullptr)
        throw MediaError("Dall'estensione non si capisce in che formato salvare: " + outputPath);
    OutputPtr output(rawOutput);

    // Flussi da copiare: video (non le copertine) e audio, con gli stessi parametri dei codec.
    std::vector<StreamCopy> streams(input->nb_streams);
    int pacingStream = findMainStream(input.get(), AVMEDIA_TYPE_VIDEO);
    if (pacingStream < 0)
        pacingStream = findMainStream(input.get(), AVMEDIA_TYPE_AUDIO);
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        AVStream* in = input->streams[i];
        const AVCodecParameters* par = in->codecpar;
        const bool video = par->codec_type == AVMEDIA_TYPE_VIDEO && (in->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
        const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !audio) {
            in->discard = AVDISCARD_ALL;
            continue;
        }
        if (avformat_query_codec(output->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            throw MediaError("Il formato " + std::string(output->oformat->name) + " non può contenere il codec "
                             + avcodec_get_name(par->codec_id) + ".");

        AVStream* out = avformat_new_stream(output.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, par) < 0)
            throw MediaError("Memoria insufficiente per preparare il file di destinazione.");
        out->codecpar->codec_tag = codecTagFor(output->oformat, par);
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);

        StreamCopy& copy = streams[i];
        copy.output = out->index;
        copy.video = video;
        copy.offset = std::llround((inSeconds + start) / av_q2d(in->time_base));
    }
    const int streamCount = static_cast<int>(output->nb_streams);
    if (streamCount == 0)
        throw MediaError("Il file non contiene video o audio da copiare.");

    // Da qui in poi, se qualcosa va storto, il file a metà va cancellato.
    const auto removePartial = [&] {
        output.reset();
        std::error_code ec;
        std::filesystem::remove(pathFromUtf8(outputPath), ec);
    };

    try {
        if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
            if (const int ret = avio_open(&output->pb, outputPath.c_str(), AVIO_FLAG_WRITE); ret < 0)
                throw MediaError("Impossibile creare il file di destinazione (" + ffmpegError(ret) + "): " + outputPath);
        }
        if (const int ret = avformat_write_header(output.get(), nullptr); ret < 0)
            throw MediaError("Il formato di destinazione non accetta questi flussi (" + ffmpegError(ret) + ").");

        // Ci si posiziona sul fotogramma chiave in IN (o quello subito prima).
        if (pacingStream >= 0) {
            const AVStream* st = input->streams[pacingStream];
            const auto ts = std::llround((inSeconds + start) / av_q2d(st->time_base));
            if (av_seek_frame(input.get(), pacingStream, ts, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(input.get(), pacingStream, 0, AVSEEK_FLAG_BACKWARD);  // si legge dall'inizio
        }

        double firstTime = std::numeric_limits<double>::max();
        double lastEnd = 0.0;
        const auto write = [&](AVPacket* pkt, StreamCopy& copy, const AVStream* in) {
            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts -= copy.offset;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts -= copy.offset;
            const AVStream* out = output->streams[copy.output];
            av_packet_rescale_ts(pkt, in->time_base, out->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) {
                const double t = pkt->pts * av_q2d(out->time_base);
                firstTime = std::min(firstTime, t);
                lastEnd = std::max(lastEnd, t + pkt->duration * av_q2d(out->time_base));
            }
            pkt->stream_index = copy.output;
            pkt->pos = -1;
            if (const int ret = av_interleaved_write_frame(output.get(), pkt); ret < 0)
                throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
        };

        PacketPtr packet(av_packet_alloc());
        if (!packet)
            throw MediaError("Memoria insufficiente per l'esportazione.");
        const double length = outSeconds - inSeconds;
        int remaining = streamCount;

        while (remaining > 0) {
            const int ret = av_read_frame(input.get(), packet.get());
            if (ret == AVERROR_EOF)
                break;
            if (ret < 0)
                throw MediaError("Errore durante la lettura del file originale (" + ffmpegError(ret) + ").");

            StreamCopy& copy = streams[packet->stream_index];
            const AVStream* in = input->streams[packet->stream_index];
            const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (copy.output < 0 || copy.finished || ts == AV_NOPTS_VALUE) {
                av_packet_unref(packet.get());
                continue;
            }
            const double tb = av_q2d(in->time_base);
            const double time = ts * tb - start;
            const double decodeTime = (packet->dts != AV_NOPTS_VALUE ? packet->dts : ts) * tb - start;

            if (decodeTime >= outSeconds + kReadPastOut) {
                // Questo flusso è finito: il video tenuto da parte oltre OUT non serve.
                copy.finished = true;
                copy.pending.clear();
                --remaining;
                av_packet_unref(packet.get());
                continue;
            }

            if (copy.video) {
                if (!copy.started) {
                    if ((packet->flags & AV_PKT_FLAG_KEY) == 0 || time < inSeconds - kEpsilon) {
                        av_packet_unref(packet.get());
                        continue;
                    }
                    copy.started = true;
                }
                if (time < inSeconds - kEpsilon) {
                    // B-frame che precedono il fotogramma chiave (GOP aperto): senza il GOP prima non si decodificano.
                    av_packet_unref(packet.get());
                    continue;
                }
                if (time >= outSeconds - kEpsilon) {
                    // Oltre OUT: lo teniamo da parte e lo scriviamo solo se poi arriva un fotogramma prima di OUT
                    // (che potrebbe usarlo come riferimento).
                    PacketPtr held(av_packet_alloc());
                    av_packet_move_ref(held.get(), packet.get());
                    copy.pending.push_back(std::move(held));
                    continue;
                }
                for (PacketPtr& held : copy.pending)
                    write(held.get(), copy, in);
                copy.pending.clear();
            } else if (time < inSeconds - kEpsilon || time >= outSeconds - kEpsilon) {
                av_packet_unref(packet.get());
                continue;
            }

            write(packet.get(), copy, in);
            if (progress && !progress(std::clamp((time - inSeconds) / length, 0.0, 1.0)))
                throw ExportCancelled();
        }

        if (const int ret = av_write_trailer(output.get()); ret < 0)
            throw MediaError("Errore nel completare il file (" + ffmpegError(ret) + ").");
        if (progress)
            progress(1.0);

        ExportResult result;
        result.durationSeconds = firstTime == std::numeric_limits<double>::max() ? 0.0 : lastEnd - firstTime;
        result.streamCount = streamCount;
        return result;
    } catch (...) {
        removePartial();
        throw;
    }
}

ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, const CutSegment& segment,
                           const ExportProgress& progress)
{
    if (!segment.hasIn() || !segment.hasOut())
        throw MediaError("Segna IN e OUT prima di esportare il segmento.");
    return exportSegment(inputPath, outputPath, segment.in(), segment.out(), progress);
}

}  // namespace editor::core
