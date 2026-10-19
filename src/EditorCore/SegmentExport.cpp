#include "EditorCore/SegmentExport.h"

#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/H264Params.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

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
    // Silenzio per i tratti senza audio
    int pcmSilence = 0;                 // PCM: valore di un byte di silenzio
    bool silenceTried = false;          // audio compresso: silenzio già preparato (o tentato)
    std::vector<std::uint8_t> silence;  // un blocco di silenzio codificato; vuoto = non disponibile
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

// Taglia un pacchetto PCM alla parte dentro `range` (al campione). false se non ne resta niente.
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

// Come si taglia e si mette in silenzio un flusso audio.
void describeAudio(StreamCopy& copy, const AVCodecParameters* par)
{
    // PCM semplice (es. WAV): i pacchetti si possono tagliare al campione esatto.
    const int bits = av_get_exact_bits_per_sample(par->codec_id);
    if (bits > 0 && par->codec_id >= AV_CODEC_ID_PCM_S16LE && par->codec_id < AV_CODEC_ID_ADPCM_IMA_QT
        && par->ch_layout.nb_channels > 0 && par->sample_rate > 0) {
        copy.pcmBlockAlign = par->ch_layout.nb_channels * bits / 8;
        copy.sampleRate = par->sample_rate;
        // Valore del silenzio: 0 per quasi tutti i formati PCM, non per questi.
        if (par->codec_id == AV_CODEC_ID_PCM_U8)
            copy.pcmSilence = 0x80;
        else if (par->codec_id == AV_CODEC_ID_PCM_MULAW)
            copy.pcmSilence = 0xFF;
        else if (par->codec_id == AV_CODEC_ID_PCM_ALAW)
            copy.pcmSilence = 0xD5;
    } else if (par->frame_size > 0 && par->sample_rate > 0) {
        copy.audioFrameSeconds = static_cast<double>(par->frame_size) / par->sample_rate;
    }
}

// withAudio = false: l'audio del file non si copia (al suo posto c'è una traccia sostitutiva).
InputFile openClip(const std::string& path, bool withAudio)
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
        const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO && withAudio;
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
        } else {
            describeAudio(copy, par);
        }
    }
    return file;
}

// Un blocco di silenzio per un flusso audio compresso, codificato con lo stesso codec e gli stessi parametri
// (frequenza, canali, profilo): il decoder dell'originale lo legge come il resto del flusso. Codificare il silenzio
// non tocca l'audio originale, che resta copiato così com'è. std::nullopt se qui non si sa fare (codec senza encoder,
// o parametri che l'encoder non riproduce).
std::optional<std::vector<std::uint8_t>> encodeSilentFrame(const AVCodecParameters* par)
{
    switch (par->codec_id) {
    case AV_CODEC_ID_AAC:
        // L'encoder fa solo AAC-LC (quello di telefoni, videocamere e registrazioni dello schermo).
        if (par->profile != AV_PROFILE_AAC_LOW && par->profile != AV_PROFILE_UNKNOWN)
            return std::nullopt;
        break;
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_EAC3:
        break;
    default:
        return std::nullopt;
    }
    const AVCodec* codec = avcodec_find_encoder(par->codec_id);
    if (codec == nullptr || par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0)
        return std::nullopt;

    // Prima con il bitrate dell'originale, poi con quello predefinito dell'encoder.
    for (const std::int64_t bitRate : {par->bit_rate, std::int64_t{0}}) {
        CodecContextPtr ctx(avcodec_alloc_context3(codec));
        FramePtr frame(av_frame_alloc());
        PacketPtr packet(av_packet_alloc());
        if (!ctx || !frame || !packet)
            return std::nullopt;
        ctx->sample_rate = par->sample_rate;
        if (par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&ctx->ch_layout, par->ch_layout.nb_channels);
        else if (av_channel_layout_copy(&ctx->ch_layout, &par->ch_layout) < 0)
            return std::nullopt;
        const void* formats = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(ctx.get(), codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count) < 0
            || formats == nullptr || count == 0)
            return std::nullopt;
        ctx->sample_fmt = static_cast<const AVSampleFormat*>(formats)[0];
        ctx->bit_rate = bitRate;
        ctx->time_base = {1, par->sample_rate};
        if (par->codec_id == AV_CODEC_ID_AAC) {
            ctx->profile = AV_PROFILE_AAC_LOW;
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
            continue;
        if (ctx->frame_size <= 0 || (par->frame_size > 0 && ctx->frame_size != par->frame_size))
            return std::nullopt;
        if (par->codec_id == AV_CODEC_ID_AAC && par->extradata_size >= 2) {
            // Tipo, frequenza e canali (i primi 13 bit della configurazione AAC) devono coincidere.
            const auto head = [](const std::uint8_t* d) { return ((d[0] << 8) | d[1]) & 0xFFF8; };
            if (ctx->extradata_size < 2 || head(ctx->extradata) != head(par->extradata))
                return std::nullopt;
        }

        // Si codificano alcuni blocchi di silenzio (l'encoder ne trattiene qualcuno all'inizio) e si tiene l'ultimo.
        std::vector<std::uint8_t> last;
        for (int i = 0; i < 8; ++i) {
            frame->nb_samples = ctx->frame_size;
            frame->format = ctx->sample_fmt;
            frame->sample_rate = ctx->sample_rate;
            if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0 || av_frame_get_buffer(frame.get(), 0) < 0)
                return std::nullopt;
            av_samples_set_silence(frame->extended_data, 0, frame->nb_samples, ctx->ch_layout.nb_channels, ctx->sample_fmt);
            frame->pts = static_cast<std::int64_t>(i) * ctx->frame_size;
            if (avcodec_send_frame(ctx.get(), frame.get()) < 0)
                return std::nullopt;
            av_frame_unref(frame.get());
            while (avcodec_receive_packet(ctx.get(), packet.get()) == 0) {
                last.assign(packet->data, packet->data + packet->size);
                av_packet_unref(packet.get());
            }
        }
        if (last.empty())
            return std::nullopt;
        return last;
    }
    return std::nullopt;
}

// AAC con l'intestazione ADTS in ogni pacchetto (es. nei file .ts): la si aggiunge anche al blocco di silenzio.
std::vector<std::uint8_t> withAdtsHeader(const std::vector<std::uint8_t>& raw, const AVCodecParameters* par)
{
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    const auto rate = std::find(std::begin(rates), std::end(rates), par->sample_rate);
    if (rate == std::end(rates))
        return raw;
    const int index = static_cast<int>(rate - std::begin(rates));
    const int channels = par->ch_layout.nb_channels == 8 ? 7 : par->ch_layout.nb_channels;
    const std::size_t size = raw.size() + 7;
    std::vector<std::uint8_t> out = {0xFF,
                                     0xF1,
                                     static_cast<std::uint8_t>((1 << 6) | (index << 2) | ((channels >> 2) & 1)),
                                     static_cast<std::uint8_t>(((channels & 3) << 6) | ((size >> 11) & 3)),
                                     static_cast<std::uint8_t>((size >> 3) & 0xFF),
                                     static_cast<std::uint8_t>(((size & 7) << 5) | 0x1F),
                                     0xFC};
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

// MP3 / MP2: un pacchetto è un fotogramma che inizia con l'intestazione (sincronismo 0xFFE).
bool isSilenceableMpegFrame(const AVPacket* pkt, const AVCodecParameters* par)
{
    return (par->codec_id == AV_CODEC_ID_MP3 || par->codec_id == AV_CODEC_ID_MP2) && pkt->size > 36 && pkt->data[0] == 0xFF
        && (pkt->data[1] & 0xE0) == 0xE0;
}

// Fotogramma MP3 / MP2 di silenzio, della stessa misura: si tiene l'intestazione (stessa frequenza, canali e
// bitrate), si toglie il controllo CRC e si azzera tutto il resto. Per MP3 vuol dire nessun dato per nessuna banda
// (lunghezze a zero nelle informazioni laterali), per MP2 nessun bit assegnato: in entrambi i casi silenzio. Non serve
// un encoder (quello MP3 di solito non c'è).
void silenceMpegFrame(AVPacket* pkt)
{
    if (av_packet_make_writable(pkt) < 0)
        throw MediaError("Memoria insufficiente per l'esportazione.");
    pkt->data[1] |= 0x01;  // protection_bit = 1: niente CRC
    std::memset(pkt->data + 4, 0, static_cast<std::size_t>(pkt->size) - 4);
}

bool hasAdtsHeader(const AVPacket* pkt)
{
    return pkt->size >= 7 && pkt->data[0] == 0xFF && (pkt->data[1] & 0xF6) == 0xF0;
}

// Audio in un tratto silenziato ([seqStart, seqEnd) = dove cade il pacchetto nella sequenza). Il PCM viene azzerato
// al campione; un pacchetto compresso il cui centro cade nel tratto prende il contenuto di un blocco di silenzio
// codificato con gli stessi parametri (stessi tempi e durata, così nel file creato l'audio resta continuo), oppure,
// con MuteMethod::Omit o se il silenzio non si sa codificare, viene tolto. false = pacchetto da non scrivere.
bool applyMute(AVPacket* pkt, StreamCopy& copy, const AVCodecParameters* par, double seqStart, double seqEnd,
               const std::vector<TimeRange>& muted, MuteMethod method, ExportResult& stats)
{
    if (muted.empty())
        return true;
    if (copy.pcmBlockAlign > 0) {
        const int samples = pkt->size / copy.pcmBlockAlign;
        bool writable = false;
        for (const TimeRange& m : muted) {
            const int first = std::clamp(static_cast<int>(std::ceil((m.start - seqStart) * copy.sampleRate - 1e-6)), 0, samples);
            const int last = std::clamp(static_cast<int>(std::ceil((m.end - seqStart) * copy.sampleRate - 1e-6)), 0, samples);
            if (last <= first)
                continue;
            if (!writable && av_packet_make_writable(pkt) < 0)
                throw MediaError("Memoria insufficiente per l'esportazione.");
            writable = true;
            std::memset(pkt->data + static_cast<std::ptrdiff_t>(first) * copy.pcmBlockAlign, copy.pcmSilence,
                        static_cast<std::size_t>(last - first) * copy.pcmBlockAlign);
        }
        if (writable)
            ++stats.silencedPackets;
        return true;
    }

    const double middle = (seqStart + std::max(seqEnd, seqStart)) / 2;
    if (std::none_of(muted.begin(), muted.end(), [&](const TimeRange& m) { return middle >= m.start && middle < m.end; }))
        return true;
    if (method == MuteMethod::Silence && isSilenceableMpegFrame(pkt, par)) {
        // MP3 e MP2: lo stesso fotogramma con i dati azzerati è silenzio (vedi silenceMpegFrame).
        silenceMpegFrame(pkt);
        ++stats.silencedPackets;
        return true;
    }
    if (method == MuteMethod::Silence && !copy.silenceTried) {
        copy.silenceTried = true;
        if (auto silence = encodeSilentFrame(par))
            copy.silence = hasAdtsHeader(pkt) ? withAdtsHeader(*silence, par) : std::move(*silence);
    }
    if (method == MuteMethod::Omit || copy.silence.empty()) {
        ++stats.droppedPackets;
        return false;
    }
    PacketPtr part(av_packet_alloc());
    if (!part || av_new_packet(part.get(), static_cast<int>(copy.silence.size())) < 0)
        throw MediaError("Memoria insufficiente per l'esportazione.");
    std::copy(copy.silence.begin(), copy.silence.end(), part->data);
    part->pts = pkt->pts;
    part->dts = pkt->dts;
    part->duration = pkt->duration;
    part->flags = pkt->flags;
    part->stream_index = pkt->stream_index;
    av_packet_unref(pkt);
    av_packet_move_ref(pkt, part.get());
    ++stats.silencedPackets;
    return true;
}

// La traccia audio sostitutiva, letta man mano che si scrive il video.
struct ReplacementSource
{
    FormatContextPtr ctx;
    int stream = -1;
    double start = 0.0;   // inizio del file
    double offset = 0.0;  // tempo nel file creato = tempo nel file audio + offset
    StreamCopy copy;
    PacketPtr pending;    // letto ma non ancora scritto
    bool ended = false;
};

std::string fileNameOf(const std::string& utf8Path)
{
    const auto name = pathFromUtf8(utf8Path).filename().u8string();
    return std::string(name.begin(), name.end());
}

}  // namespace

namespace {

// Prova a scrivere un file del formato `format` con il solo flusso `in` e qualche suo pacchetto (in un file
// temporaneo): alcuni contenitori dichiarano di accettare un codec ma poi non riescono a chiudere il file.
bool canMux(const AVOutputFormat* format, const std::string& extension, AVFormatContext* input, int streamIndex)
{
    std::error_code ec;
    static std::atomic<int> counter{0};
    const std::filesystem::path probe = std::filesystem::temp_directory_path(ec)
                                      / ("EditorVideoProvaAudio" + std::to_string(++counter) + extension);
    const std::string probePath = [&] {
        const auto text = probe.u8string();
        return std::string(text.begin(), text.end());
    }();
    bool ok = false;
    {
        AVFormatContext* raw = nullptr;
        if (avformat_alloc_output_context2(&raw, format, nullptr, probePath.c_str()) < 0 || raw == nullptr)
            return false;
        OutputPtr out(raw);
        const AVStream* in = input->streams[streamIndex];
        AVStream* stream = avformat_new_stream(out.get(), nullptr);
        PacketPtr packet(av_packet_alloc());
        if (stream == nullptr || !packet || avcodec_parameters_copy(stream->codecpar, in->codecpar) < 0)
            return false;
        stream->codecpar->codec_tag = codecTagFor(format, in->codecpar);
        stream->time_base = in->time_base;
        if ((format->flags & AVFMT_NOFILE) == 0 && avio_open(&out->pb, probePath.c_str(), AVIO_FLAG_WRITE) < 0)
            return false;
        ok = avformat_write_header(out.get(), nullptr) >= 0;
        for (int written = 0; ok && written < 3 && av_read_frame(input, packet.get()) >= 0;) {
            if (packet->stream_index == streamIndex) {
                av_packet_rescale_ts(packet.get(), in->time_base, stream->time_base);
                packet->stream_index = 0;
                packet->pos = -1;
                ok = av_interleaved_write_frame(out.get(), packet.get()) >= 0;
                ++written;
            }
            av_packet_unref(packet.get());
        }
        ok = ok && av_write_trailer(out.get()) >= 0;
    }
    std::filesystem::remove(probe, ec);
    return ok;
}

}  // namespace

std::string replacementAudioProblem(const std::string& audioPath, const std::string& outputPath)
{
    FormatContextPtr ctx;
    try {
        ctx = openInput(audioPath);
    } catch (const std::exception& e) {
        return e.what();
    }
    const int index = findMainStream(ctx.get(), AVMEDIA_TYPE_AUDIO);
    if (index < 0)
        return "Il file " + fileNameOf(audioPath) + " non contiene audio.";
    const AVOutputFormat* format = av_guess_format(nullptr, outputPath.c_str(), nullptr);
    if (format == nullptr)
        return {};  // lo dirà l'esportazione
    const AVCodecID codec = ctx->streams[index]->codecpar->codec_id;
    if (avformat_query_codec(format, codec, FF_COMPLIANCE_NORMAL) == 0 || !canMux(format, pathFromUtf8(outputPath).extension().string(), ctx.get(), index)) {
        std::string extension = pathFromUtf8(outputPath).extension().string();
        if (!extension.empty())
            extension.erase(0, 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return "L'audio di " + fileNameOf(audioPath) + " (" + avcodec_get_name(codec) + ") non può stare in un file "
             + extension + " senza ricodifica. Scegli un audio in un formato adatto (per MP4 e MOV: AAC, M4A o MP3), "
               "oppure monta file MKV, che accettano quasi tutti i formati audio.";
    }
    return {};
}

ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath,
                         const ExportProgress& progress)
{
    return exportClips(clips, outputPath, AudioEdit{}, progress);
}

ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                         const ExportProgress& progress, MuteMethod muteMethod)
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
    const bool replaced = audio.replacement.has_value();
    if (replaced) {
        if (samePath(audio.replacement->path, outputPath))
            throw MediaError("Il file di destinazione deve essere diverso dall'audio sostitutivo.");
        if (const std::string problem = replacementAudioProblem(audio.replacement->path, outputPath); !problem.empty())
            throw MediaError(problem);
    }

    // Più file: devono avere gli stessi codec e parametri, altrimenti senza ricodifica non si uniscono. Con una
    // traccia sostitutiva l'audio dei file non si copia, quindi conta solo il video.
    std::vector<MediaSignature> signatures;
    if (clips.size() > 1) {
        for (const ClipRanges& clip : clips) {
            signatures.push_back(readMediaSignature(clip.path));
            if (replaced)
                std::erase_if(signatures.back().streams, [](const StreamSignature& s) { return s.kind == StreamKind::Audio; });
        }
        if (const auto mismatches = checkCompatibility(signatures); !mismatches.empty())
            throw MediaError("Questi file non si possono unire senza ricodifica:\n" + describeMismatches(mismatches));
    }

    InputFile first = openClip(clips.front().path, !replaced);
    const int clipStreams = first.copied;
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

    // Traccia sostitutiva: un flusso audio in più, copiato dal file audio.
    ReplacementSource replacement;
    if (replaced) {
        replacement.ctx = openInput(audio.replacement->path);
        replacement.stream = findMainStream(replacement.ctx.get(), AVMEDIA_TYPE_AUDIO);
        if (replacement.stream < 0)
            throw MediaError("Il file " + fileNameOf(audio.replacement->path) + " non contiene audio.");
        for (unsigned i = 0; i < replacement.ctx->nb_streams; ++i) {
            if (static_cast<int>(i) != replacement.stream)
                replacement.ctx->streams[i]->discard = AVDISCARD_ALL;
        }
        replacement.start = startOffsetSeconds(replacement.ctx.get());
        replacement.offset = audio.replacement->offset;
        const AVStream* in = replacement.ctx->streams[replacement.stream];
        describeAudio(replacement.copy, in->codecpar);
        AVStream* out = avformat_new_stream(output.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
            throw MediaError("Memoria insufficiente per preparare il file di destinazione.");
        out->codecpar->codec_tag = codecTagFor(output->oformat, in->codecpar);
        out->time_base = in->time_base;
        out->disposition = AV_DISPOSITION_DEFAULT;
        replacement.copy.output = out->index;
        // Si parte dal punto del file audio che finisce all'inizio del file creato.
        const auto ts = std::llround((std::max(0.0, -replacement.offset) + replacement.start) / av_q2d(in->time_base));
        if (av_seek_frame(replacement.ctx.get(), replacement.stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(replacement.ctx.get(), replacement.stream, 0, AVSEEK_FLAG_BACKWARD);
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

        ExportResult result;
        // Per ogni flusso del file creato: ultimo tempo di decodifica scritto (devono sempre crescere).
        std::vector<std::int64_t> lastDts(streamCount, std::numeric_limits<std::int64_t>::min());
        double doneLength = 0.0;
        double outputPosition = 0.0;  // dove inizia, nel file creato, il tratto in corso
        double firstTime = std::numeric_limits<double>::max();
        double lastEnd = 0.0;

        // I tempi di decodifica devono sempre crescere, anche a cavallo di una giunzione (con i B-frame il primo
        // fotogramma di un tratto può avere un tempo di decodifica appena prima della fine del tratto precedente):
        // se serve si sposta avanti di un'unità.
        const auto keepDtsIncreasing = [&](AVPacket* pkt, int outputIndex) {
            std::int64_t& last = lastDts[outputIndex];
            if (pkt->dts == AV_NOPTS_VALUE)
                return;
            if (last != std::numeric_limits<std::int64_t>::min() && pkt->dts <= last) {
                pkt->dts = last + 1;
                if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
                    pkt->pts = pkt->dts;
            }
            last = pkt->dts;
        };

        // Scrive la traccia sostitutiva fino al tempo `until` del file creato, senza superare `end` (la fine del
        // file creato): si chiama man mano che si scrive il video, così video e audio restano mescolati nel file.
        const auto pumpReplacement = [&](double until, double end) {
            if (!replaced)
                return;
            AVFormatContext* input = replacement.ctx.get();
            const AVStream* in = input->streams[replacement.stream];
            const AVStream* out = output->streams[replacement.copy.output];
            const double tb = av_q2d(in->time_base);
            while (!replacement.ended) {
                if (!replacement.pending) {
                    PacketPtr next(av_packet_alloc());
                    if (!next)
                        throw MediaError("Memoria insufficiente per l'esportazione.");
                    const int ret = av_read_frame(input, next.get());
                    if (ret == AVERROR_EOF) {
                        replacement.ended = true;
                        break;
                    }
                    if (ret < 0)
                        throw MediaError("Errore durante la lettura dell'audio sostitutivo (" + ffmpegError(ret) + ").");
                    if (next->stream_index != replacement.stream || (next->pts == AV_NOPTS_VALUE && next->dts == AV_NOPTS_VALUE))
                        continue;
                    replacement.pending = std::move(next);
                }
                AVPacket* pkt = replacement.pending.get();
                const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                const double sourceTime = ts * tb - replacement.start;
                double time = sourceTime + replacement.offset;  // nel file creato
                if (time >= until)
                    break;
                if (time >= end - kEpsilon) {
                    replacement.ended = true;
                    break;
                }
                PacketPtr current = std::move(replacement.pending);
                double duration = std::max<std::int64_t>(pkt->duration, 0) * tb;
                if (replacement.copy.pcmBlockAlign > 0) {
                    if (!trimPcm(pkt, replacement.copy, sourceTime, tb, {-replacement.offset, end - replacement.offset}))
                        continue;
                    time = (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts) * tb - replacement.start + replacement.offset;
                    duration = pkt->duration * tb;
                } else if (time < -kEpsilon || time + duration > end + kEpsilon) {
                    continue;  // solo i pacchetti interamente dentro il file creato
                }
                if (!applyMute(pkt, replacement.copy, in->codecpar, time, time + duration, audio.muted, muteMethod, result))
                    continue;
                const auto shift = static_cast<std::int64_t>(std::llround((replacement.start - replacement.offset) / tb));
                if (pkt->pts != AV_NOPTS_VALUE)
                    pkt->pts -= shift;
                if (pkt->dts != AV_NOPTS_VALUE)
                    pkt->dts -= shift;
                av_packet_rescale_ts(pkt, in->time_base, out->time_base);
                keepDtsIncreasing(pkt, replacement.copy.output);
                firstTime = std::min(firstTime, time);
                lastEnd = std::max(lastEnd, time + duration);
                pkt->stream_index = replacement.copy.output;
                pkt->pos = -1;
                if (const int ret = av_interleaved_write_frame(output.get(), pkt); ret < 0)
                    throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
            }
        };

        for (std::size_t c = 0; c < clips.size(); ++c) {
            InputFile current = c == 0 ? std::move(first) : openClip(clips[c].path, !replaced);
            if (current.copied != clipStreams)
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
                    keepDtsIncreasing(pkt, copy.output);

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
                        // La traccia sostitutiva va scritta insieme al video, poco più avanti.
                        if (copy.video)
                            pumpReplacement(std::min(t + 0.3, totalLength - 0.1), totalLength);
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
                int remaining = clipStreams;

                // Ci si posiziona sul fotogramma chiave all'inizio del tratto (o quello subito prima).
                if (current.pacingStream >= 0 && remaining > 0) {
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
                    } else {
                        if (copy.pcmBlockAlign > 0) {
                            // Audio PCM: si tiene esattamente la parte del pacchetto dentro il tratto.
                            if (!trimPcm(packet.get(), copy, time, tb, range)) {
                                av_packet_unref(packet.get());
                                continue;
                            }
                        } else {
                            // Audio compresso: solo i pacchetti interamente dentro il tratto, così alle giunzioni non
                            // si sovrappone (al più un pacchetto, ~20 ms, di silenzio a ogni giunzione).
                            const double end = time + std::max<std::int64_t>(packet->duration, 0) * tb;
                            if (time < range.start - kEpsilon || end > range.end + kEpsilon) {
                                av_packet_unref(packet.get());
                                continue;
                            }
                        }
                        // Tratti silenziati: si confrontano con il tempo del pacchetto nella sequenza.
                        const std::int64_t kept = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                        const double seqStart = doneLength + (kept * tb - start - range.start);
                        double length = std::max<std::int64_t>(packet->duration, 0) * tb;
                        if (length <= 0)
                            length = copy.audioFrameSeconds;
                        if (!applyMute(packet.get(), copy, in->codecpar, seqStart, seqStart + length, audio.muted,
                                       muteMethod, result)) {
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
        // Il resto della traccia sostitutiva, fino alla fine del file creato.
        pumpReplacement(std::numeric_limits<double>::max(), outputPosition);

        if (const int ret = av_write_trailer(output.get()); ret < 0)
            throw MediaError("Errore nel completare il file (" + ffmpegError(ret) + ").");
        if (progress)
            progress(1.0);

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
