#include "EditorCore/SegmentExport.h"

#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/H264Params.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace editor::core {

using namespace detail;

namespace {

constexpr double kEpsilon = 0.001;
// Dopo la fine di un tratto si legge ancora un po': in un file i pacchetti dei vari flussi sono mescolati e,
// con i B-frame, un fotogramma prima della fine può arrivare dopo uno successivo.
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
    double frameDuration = 0.0;  // video: durata di un fotogramma, se il pacchetto non la dice
    int pcmBlockAlign = 0;       // audio PCM: byte per campione (tutti i canali); 0 = audio compresso
    double audioFrameSeconds = 0.0;  // audio compresso: durata di un blocco decodificato (AAC: 1024 campioni)
    int sampleRate = 0;
    // Stato per il tratto in corso
    bool started = false;  // video: trovato il fotogramma chiave di partenza
    bool finished = false;
    std::vector<PacketPtr> pending;  // video oltre la fine tenuto da parte (può servire come riferimento)

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

// Taglia un pacchetto PCM alla parte dentro ange (al campione). false se non ne resta niente.
bool trimPcm(AVPacket* pkt, const StreamCopy& copy, double time, double tb, const TimeRange& range)
{
    const int samples = pkt->size / copy.pcmBlockAlign;
    const double rate = copy.sampleRate;
    const int first = std::clamp(static_cast<int>(std::ceil((range.start - time) * rate - 1e-6)), 0, samples);
    const int last = std::clamp(static_cast<int>(std::floor((range.end - time) * rate + 1e-6)), 0, samples);
    if (last <= first)
        return false;
    if (first == 0 && last == samples)
        return true;

    PacketPtr part(av_packet_alloc());
    if (!part || av_new_packet(part.get(), (last - first) * copy.pcmBlockAlign) < 0)
        throw MediaError("Memoria insufficiente per l'esportazione.");
    std::memcpy(part->data, pkt->data + static_cast<std::ptrdiff_t>(first) * copy.pcmBlockAlign,
                static_cast<std::size_t>(last - first) * copy.pcmBlockAlign);
    const auto offset = static_cast<std::int64_t>(std::llround(first / rate / tb));
    part->pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts + offset : AV_NOPTS_VALUE;
    part->dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts + offset : AV_NOPTS_VALUE;
    part->duration = std::llround((last - first) / rate / tb);
    part->flags = pkt->flags;
    part->stream_index = pkt->stream_index;
    av_packet_unref(pkt);
    av_packet_move_ref(pkt, part.get());
    return true;
}

}  // namespace

ExportCancelled::ExportCancelled()
    : MediaError("Esportazione annullata.")
{
}

namespace {

// Un file da cui si copia: i flussi video e audio vanno, nell'ordine, nei flussi 0, 1, 2... del file creato.
struct InputFile
{
    FormatContextPtr ctx;
    double start = 0.0;
    int pacingStream = -1;
    std::vector<StreamCopy> streams;  // per indice del flusso nel file
    int copied = 0;                   // flussi video + audio
};

InputFile openClip(const std::string& path)
{
    InputFile file;
    file.ctx = openInput(path);
    file.start = startOffsetSeconds(file.ctx.get());
    file.pacingStream = findMainStream(file.ctx.get(), AVMEDIA_TYPE_VIDEO);
    if (file.pacingStream < 0)
        file.pacingStream = findMainStream(file.ctx.get(), AVMEDIA_TYPE_AUDIO);
    file.streams.resize(file.ctx->nb_streams);

    for (unsigned i = 0; i < file.ctx->nb_streams; ++i) {
        AVStream* in = file.ctx->streams[i];
        const AVCodecParameters* par = in->codecpar;
        const bool video = par->codec_type == AVMEDIA_TYPE_VIDEO && (in->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
        const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !audio) {
            in->discard = AVDISCARD_ALL;
            continue;
        }
        StreamCopy& copy = file.streams[i];
        copy.output = file.copied++;
        copy.video = video;
        if (video) {
            const AVRational rate = av_guess_frame_rate(file.ctx.get(), in, nullptr);
            copy.frameDuration = rate.num > 0 && rate.den > 0 ? 1.0 / av_q2d(rate) : 0.0;
        }
        // PCM semplice (es. WAV): i pacchetti si possono tagliare al campione esatto.
        const int bits = av_get_exact_bits_per_sample(par->codec_id);
        if (audio && bits > 0 && par->codec_id >= AV_CODEC_ID_PCM_S16LE && par->codec_id < AV_CODEC_ID_ADPCM_IMA_QT
            && par->ch_layout.nb_channels > 0 && par->sample_rate > 0) {
            copy.pcmBlockAlign = par->ch_layout.nb_channels * bits / 8;
            copy.sampleRate = par->sample_rate;
        } else if (audio && par->frame_size > 0 && par->sample_rate > 0) {
            copy.audioFrameSeconds = static_cast<double>(par->frame_size) / par->sample_rate;
        }
    }
    return file;
}

}  // namespace

ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath,
                         const ExportProgress& progress)
{
    double totalLength = 0.0;
    int rangeCount = 0;
    for (const ClipRanges& clip : clips) {
        for (const TimeRange& range : clip.ranges) {
            if (!(range.end > range.start + kEpsilon))
                throw MediaError("OUT deve stare dopo IN.");
            totalLength += range.length();
            ++rangeCount;
        }
        if (samePath(clip.path, outputPath))
            throw MediaError("Il file di destinazione deve essere diverso dagli originali.");
    }
    if (rangeCount == 0)
        throw MediaError("Non c'è nessun tratto da esportare.");

    // Più file: devono avere gli stessi codec e parametri, altrimenti senza ricodifica non si uniscono.
    std::vector<MediaSignature> signatures;
    if (clips.size() > 1) {
        for (const ClipRanges& clip : clips)
            signatures.push_back(readMediaSignature(clip.path));
        if (const auto mismatches = checkCompatibility(signatures); !mismatches.empty())
            throw MediaError("Questi file non si possono unire senza ricodifica:\n" + describeMismatches(mismatches));
    }

    InputFile first = openClip(clips.front().path);
    AVFormatContext* rawOutput = nullptr;
    if (avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, outputPath.c_str()) < 0 || rawOutput == nullptr)
        throw MediaError("Dall'estensione non si capisce in che formato salvare: " + outputPath);
    OutputPtr output(rawOutput);

    // Flussi del file creato: quelli del primo file, con gli stessi parametri dei codec.
    for (unsigned i = 0; i < first.ctx->nb_streams; ++i) {
        if (first.streams[i].output < 0)
            continue;
        const AVStream* in = first.ctx->streams[i];
        const AVCodecParameters* par = in->codecpar;
        if (avformat_query_codec(output->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            throw MediaError("Il formato " + std::string(output->oformat->name) + " non può contenere il codec "
                             + avcodec_get_name(par->codec_id) + ".");
        AVStream* out = avformat_new_stream(output.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, par) < 0)
            throw MediaError("Memoria insufficiente per preparare il file di destinazione.");
        out->codecpar->codec_tag = codecTagFor(output->oformat, par);
        // H.264 da file con livelli diversi: il file unito dichiara il livello più alto (il resto dei parametri
        // coincide, l'ha verificato checkCompatibility).
        if (par->codec_id == AV_CODEC_ID_H264 && !signatures.empty()) {
            const int index = first.streams[i].output;
            int level = 0;
            for (const MediaSignature& signature : signatures) {
                if (index < static_cast<int>(signature.streams.size()))
                    level = std::max(level, maxH264Level(parseH264Config(signature.streams[index].extradata)));
            }
            const std::vector<std::uint8_t> current(par->extradata, par->extradata + par->extradata_size);
            if (level > maxH264Level(parseH264Config(current))) {
                const std::vector<std::uint8_t> raised = withH264Level(current, level);
                av_freep(&out->codecpar->extradata);
                out->codecpar->extradata = static_cast<std::uint8_t*>(av_mallocz(raised.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (out->codecpar->extradata == nullptr)
                    throw MediaError("Memoria insufficiente per preparare il file di destinazione.");
                std::copy(raised.begin(), raised.end(), out->codecpar->extradata);
                out->codecpar->extradata_size = static_cast<int>(raised.size());
                out->codecpar->level = level;
            }
        }
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);
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

        PacketPtr packet(av_packet_alloc());
        if (!packet)
            throw MediaError("Memoria insufficiente per l'esportazione.");

        // Per ogni flusso del file creato: ultimo tempo di decodifica scritto (devono sempre crescere).
        std::vector<std::int64_t> lastDts(streamCount, std::numeric_limits<std::int64_t>::min());
        double doneLength = 0.0;
        double outputPosition = 0.0;  // dove inizia, nel file creato, il tratto in corso
        double firstTime = std::numeric_limits<double>::max();
        double lastEnd = 0.0;

        for (std::size_t c = 0; c < clips.size(); ++c) {
            InputFile current = c == 0 ? std::move(first) : openClip(clips[c].path);
            if (current.copied != streamCount)
                throw MediaError("Il file " + clips[c].path + " ha un numero di flussi diverso dal primo.");
            AVFormatContext* input = current.ctx.get();
            const double start = current.start;

            for (const TimeRange& range : clips[c].ranges) {
                // Tutti i tempi del tratto vengono spostati dello stesso valore, per video e audio:
                // tempo nel file creato = tempo nell'originale - range.start + outputPosition.
                const double shiftSeconds = range.start + start - outputPosition;
                double videoEnd = outputPosition;  // fine dell'ultimo fotogramma scritto di questo tratto
                double audioEnd = outputPosition;  // fine dell'audio compresso come uscirà dal decoder

                const auto write = [&](AVPacket* pkt, StreamCopy& copy, const AVStream* in) {
                    const auto shift = static_cast<std::int64_t>(std::llround(shiftSeconds / av_q2d(in->time_base)));
                    if (pkt->pts != AV_NOPTS_VALUE)
                        pkt->pts -= shift;
                    if (pkt->dts != AV_NOPTS_VALUE)
                        pkt->dts -= shift;
                    const AVStream* out = output->streams[copy.output];
                    av_packet_rescale_ts(pkt, in->time_base, out->time_base);

                    // I tempi di decodifica devono sempre crescere, anche a cavallo di una giunzione (con i B-frame
                    // il primo fotogramma di un tratto può avere un tempo di decodifica appena prima della fine del
                    // tratto precedente): se serve si sposta avanti di un'unità.
                    std::int64_t& last = lastDts[copy.output];
                    if (pkt->dts != AV_NOPTS_VALUE) {
                        if (last != std::numeric_limits<std::int64_t>::min() && pkt->dts <= last) {
                            pkt->dts = last + 1;
                            if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
                                pkt->pts = pkt->dts;
                        }
                        last = pkt->dts;
                    }

                    if (pkt->pts != AV_NOPTS_VALUE) {
                        const double t = pkt->pts * av_q2d(out->time_base);
                        double duration = pkt->duration * av_q2d(out->time_base);
                        if (copy.video && duration <= 0)
                            duration = copy.frameDuration;
                        firstTime = std::min(firstTime, t);
                        lastEnd = std::max(lastEnd, t + duration);
                        if (copy.video)
                            videoEnd = std::max(videoEnd, t + duration);
                        // Il decoder restituisce sempre un blocco intero, anche se il contenitore ne dichiara una parte
                        // (l'ultimo blocco di un file AAC contiene il riempimento dell'encoder): in mezzo al file
                        // creato quella parte in più non si può togliere, quindi se ne tiene conto.
                        if (copy.audioFrameSeconds > 0)
                            audioEnd = std::max(audioEnd, t + std::max(duration, copy.audioFrameSeconds));
                    }
                    pkt->stream_index = copy.output;
                    pkt->pos = -1;
                    if (const int ret = av_interleaved_write_frame(output.get(), pkt); ret < 0)
                        throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
                };

                for (StreamCopy& copy : current.streams) {
                    copy.started = false;
                    copy.finished = copy.output < 0;
                    copy.pending.clear();
                }
                int remaining = streamCount;

                // Ci si posiziona sul fotogramma chiave all'inizio del tratto (o quello subito prima).
                if (current.pacingStream >= 0) {
                    const AVStream* st = input->streams[current.pacingStream];
                    const auto ts = std::llround((range.start + start) / av_q2d(st->time_base));
                    if (av_seek_frame(input, current.pacingStream, ts, AVSEEK_FLAG_BACKWARD) < 0)
                        av_seek_frame(input, current.pacingStream, 0, AVSEEK_FLAG_BACKWARD);  // si legge dall'inizio
                }

                while (remaining > 0) {
                    const int ret = av_read_frame(input, packet.get());
                    if (ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        throw MediaError("Errore durante la lettura del file originale (" + ffmpegError(ret) + ").");

                    StreamCopy& copy = current.streams[packet->stream_index];
                    const AVStream* in = input->streams[packet->stream_index];
                    const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                    if (copy.output < 0 || copy.finished || ts == AV_NOPTS_VALUE) {
                        av_packet_unref(packet.get());
                        continue;
                    }
                    const double tb = av_q2d(in->time_base);
                    const double time = ts * tb - start;
                    const double decodeTime = (packet->dts != AV_NOPTS_VALUE ? packet->dts : ts) * tb - start;

                    if (decodeTime >= range.end + kReadPastOut) {
                        // Questo flusso ha finito il tratto: il video tenuto da parte oltre la fine non serve.
                        copy.finished = true;
                        copy.pending.clear();
                        --remaining;
                        av_packet_unref(packet.get());
                        continue;
                    }

                    if (copy.video) {
                        if (!copy.started) {
                            if ((packet->flags & AV_PKT_FLAG_KEY) == 0 || time < range.start - kEpsilon) {
                                av_packet_unref(packet.get());
                                continue;
                            }
                            copy.started = true;
                        }
                        if (time < range.start - kEpsilon) {
                            // B-frame che precedono il fotogramma chiave (GOP aperto): senza il GOP prima non si
                            // decodificano.
                            av_packet_unref(packet.get());
                            continue;
                        }
                        if (time >= range.end - kEpsilon) {
                            // Oltre la fine: lo teniamo da parte e lo scriviamo solo se poi arriva un fotogramma
                            // prima della fine (che potrebbe usarlo come riferimento).
                            PacketPtr held(av_packet_alloc());
                            av_packet_move_ref(held.get(), packet.get());
                            copy.pending.push_back(std::move(held));
                            continue;
                        }
                        for (PacketPtr& held : copy.pending)
                            write(held.get(), copy, in);
                        copy.pending.clear();
                    } else if (copy.pcmBlockAlign > 0) {
                        // Audio PCM: si tiene esattamente la parte del pacchetto dentro il tratto.
                        if (!trimPcm(packet.get(), copy, time, tb, range)) {
                            av_packet_unref(packet.get());
                            continue;
                        }
                    } else {
                        // Audio compresso: solo i pacchetti interamente dentro il tratto, così alle giunzioni non si
                        // sovrappone (al più un pacchetto, ~20 ms, di silenzio a ogni giunzione).
                        const double end = time + std::max<std::int64_t>(packet->duration, 0) * tb;
                        if (time < range.start - kEpsilon || end > range.end + kEpsilon) {
                            av_packet_unref(packet.get());
                            continue;
                        }
                    }

                    write(packet.get(), copy, in);
                    if (progress) {
                        const double inRange = std::clamp(time - range.start, 0.0, range.length());
                        if (!progress(std::clamp((doneLength + inRange) / totalLength, 0.0, 1.0)))
                            throw ExportCancelled();
                    }
                }

                // Il tratto successivo comincia dove finisce questo: alla sua durata, o più in là se è stato scritto
                // qualche fotogramma di riferimento oltre la fine o se l'ultimo blocco audio decodificato va oltre
                // (pochi millesimi): così video e audio ripartono insieme e l'audio non si sovrappone.
                outputPosition = std::max({outputPosition + range.length(), videoEnd, audioEnd});
                doneLength += range.length();
            }
        }

        if (const int ret = av_write_trailer(output.get()); ret < 0)
            throw MediaError("Errore nel completare il file (" + ffmpegError(ret) + ").");
        if (progress)
            progress(1.0);

        ExportResult result;
        result.durationSeconds = firstTime == std::numeric_limits<double>::max() ? 0.0 : lastEnd - firstTime;
        result.streamCount = streamCount;
        result.segmentCount = rangeCount;
        result.clipCount = static_cast<int>(clips.size());
        return result;
    } catch (...) {
        removePartial();
        throw;
    }
}

ExportResult exportSegments(const std::string& inputPath, const std::string& outputPath,
                            const std::vector<TimeRange>& ranges, const ExportProgress& progress)
{
    return exportClips({{inputPath, ranges}}, outputPath, progress);
}
ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, double inSeconds,
                           double outSeconds, const ExportProgress& progress)
{
    return exportSegments(inputPath, outputPath, {{inSeconds, outSeconds}}, progress);
}

ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, const CutSegment& segment,
                           const ExportProgress& progress)
{
    if (!segment.hasIn() || !segment.hasOut())
        throw MediaError("Segna IN e OUT prima di esportare il segmento.");
    return exportSegment(inputPath, outputPath, segment.in(), segment.out(), progress);
}

}  // namespace editor::core
