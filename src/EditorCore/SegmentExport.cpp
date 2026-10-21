#include "EditorCore/SegmentExport.h"

#include "EditorCore/Compatibility.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace editor::core {

using namespace detail;

ExportCancelled::ExportCancelled()
    : MediaError("Esportazione annullata.")
{
}

namespace {

constexpr const char* kOutOfMemory = "Memoria insufficiente per l'esportazione.";
constexpr const char* kOutOfMemoryForOutput = "Memoria insufficiente per preparare il file di destinazione.";

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

// Alla fine del blocco libera il contenuto del pacchetto, che resta allocato per essere riusato.
struct PacketUnref
{
    AVPacket* pkt;
    ~PacketUnref() { av_packet_unref(pkt); }
};

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

std::string uppercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// Sostituisce il contenuto del pacchetto con `size` byte da `data` (che possono stare dentro il pacchetto stesso),
// tenendo tempi, durata, flag e flusso.
void replacePacketData(AVPacket* pkt, const std::uint8_t* data, std::size_t size)
{
    PacketPtr part(av_packet_alloc());
    if (!part || av_new_packet(part.get(), static_cast<int>(size)) < 0)
        throw MediaError(kOutOfMemory);
    std::memcpy(part->data, data, size);
    part->pts = pkt->pts;
    part->dts = pkt->dts;
    part->duration = pkt->duration;
    part->flags = pkt->flags;
    part->stream_index = pkt->stream_index;
    av_packet_unref(pkt);
    av_packet_move_ref(pkt, part.get());
}

// Sposta indietro di `shift` (unità del flusso) i tempi del pacchetto che ci sono.
void shiftPacket(AVPacket* pkt, std::int64_t shift)
{
    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= shift;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts -= shift;
}

// Parametri di codifica di un file H.264 o HEVC, da ripetere nel flusso: i NAL (SPS e PPS, per HEVC anche VPS) e quanti
// byte misura la lunghezza davanti a ogni NAL nei pacchetti.
struct ParameterSets
{
    int lengthSize = 4;
    std::vector<std::vector<std::uint8_t>> nals;
};

// Dai "dati extra" in formato avcC / hvcC (MP4, MKV). std::nullopt se sono in un altro formato (es. Annex B).
std::optional<ParameterSets> parameterSetsOf(AVCodecID codec, const std::vector<std::uint8_t>& extra)
{
    ParameterSets out;
    std::size_t pos = 0;
    const auto be16 = [&](std::size_t at) { return static_cast<std::size_t>(extra[at] << 8 | extra[at + 1]); };
    // Un NAL preceduto da 2 byte di lunghezza; `keep` = da tenere.
    const auto takeNal = [&](bool keep) {
        if (pos + 2 > extra.size())
            return false;
        const std::size_t length = be16(pos);
        pos += 2;
        if (pos + length > extra.size())
            return false;
        if (keep)
            out.nals.emplace_back(extra.begin() + static_cast<std::ptrdiff_t>(pos),
                                  extra.begin() + static_cast<std::ptrdiff_t>(pos + length));
        pos += length;
        return true;
    };
    if (codec == AV_CODEC_ID_H264) {
        if (extra.size() < 7 || extra[0] != 1)
            return std::nullopt;
        out.lengthSize = (extra[4] & 3) + 1;
        pos = 5;
        for (int list = 0; list < 2; ++list) {  // prima gli SPS, poi i PPS
            if (pos >= extra.size())
                return std::nullopt;
            const int count = list == 0 ? (extra[pos++] & 0x1f) : extra[pos++];
            for (int i = 0; i < count; ++i) {
                if (!takeNal(true))
                    return std::nullopt;
            }
        }
        return out;
    }
    if (codec == AV_CODEC_ID_HEVC) {
        if (extra.size() < 23 || extra[0] != 1)
            return std::nullopt;
        out.lengthSize = (extra[21] & 3) + 1;
        const int arrays = extra[22];
        pos = 23;
        for (int a = 0; a < arrays; ++a) {
            if (pos + 3 > extra.size())
                return std::nullopt;
            const int type = extra[pos] & 0x3f;
            const std::size_t count = be16(pos + 1);
            pos += 3;
            for (std::size_t i = 0; i < count; ++i) {
                if (!takeNal(type == 32 || type == 33 || type == 34))  // VPS, SPS, PPS
                    return std::nullopt;
            }
        }
        return out;
    }
    return std::nullopt;
}

// Riscrive il pacchetto (NAL con la lunghezza davanti, su `fromSize` byte) con le lunghezze su `toSize` byte e, davanti,
// i NAL di `prefix`. false se il pacchetto non si capisce.
bool rewriteNals(AVPacket* pkt, int fromSize, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix)
{
    std::vector<std::uint8_t> data;
    data.reserve(static_cast<std::size_t>(pkt->size) + 256);
    const auto putLength = [&](std::size_t length) {
        for (int b = toSize - 1; b >= 0; --b)
            data.push_back(static_cast<std::uint8_t>(length >> (8 * b)));
    };
    for (const auto& nal : prefix) {
        putLength(nal.size());
        data.insert(data.end(), nal.begin(), nal.end());
    }
    std::size_t pos = 0;
    const auto size = static_cast<std::size_t>(pkt->size);
    while (pos < size) {
        if (pos + static_cast<std::size_t>(fromSize) > size)
            return false;
        std::size_t length = 0;
        for (int b = 0; b < fromSize; ++b)
            length = length << 8 | pkt->data[pos + static_cast<std::size_t>(b)];
        pos += static_cast<std::size_t>(fromSize);
        if (pos + length > size || (toSize < 4 && length >> (8 * toSize) != 0))
            return false;
        putLength(length);
        data.insert(data.end(), pkt->data + pos, pkt->data + pos + length);
        pos += length;
    }
    replacePacketData(pkt, data.data(), data.size());
    return true;
}

// NAL di un flusso Annex B (con i codici di inizio 00 00 01 / 00 00 00 01).
std::vector<std::vector<std::uint8_t>> splitAnnexB(const std::uint8_t* data, std::size_t size)
{
    std::vector<std::vector<std::uint8_t>> nals;
    std::size_t i = 0;
    std::size_t begin = std::string::npos;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (begin != std::string::npos) {
                std::size_t end = i;
                while (end > begin && data[end - 1] == 0)
                    --end;  // gli zeri prima del codice di inizio non fanno parte del NAL
                nals.emplace_back(data + begin, data + end);
            }
            i += 3;
            begin = i;
            continue;
        }
        ++i;
    }
    if (begin != std::string::npos && begin < size)
        nals.emplace_back(data + begin, data + size);
    return nals;
}

bool startsWithStartCode(const std::uint8_t* data, std::size_t size)
{
    return (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        || (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

// Parametri di codifica (SPS/PPS, per HEVC anche VPS) dai "dati extra", in avcC/hvcC o in Annex B (come li danno
// gli encoder).
std::optional<ParameterSets> parameterNalsOf(AVCodecID codec, const std::vector<std::uint8_t>& extra)
{
    if (!extra.empty() && extra[0] == 1)
        return parameterSetsOf(codec, extra);
    ParameterSets sets;
    for (auto& nal : splitAnnexB(extra.data(), extra.size())) {
        if (nal.empty())
            continue;
        const int type = codec == AV_CODEC_ID_HEVC ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
        const bool parameter = codec == AV_CODEC_ID_HEVC ? (type == 32 || type == 33 || type == 34) : (type == 7 || type == 8);
        if (parameter)
            sets.nals.push_back(std::move(nal));
    }
    if (sets.nals.empty())
        return std::nullopt;
    return sets;
}

// Pacchetto dell'encoder (Annex B, o già con le lunghezze su 4 byte) con le lunghezze su `toSize` byte e, davanti, i
// NAL di `prefix`.
bool encodedToLengthPrefixed(AVPacket* pkt, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix)
{
    if (!startsWithStartCode(pkt->data, static_cast<std::size_t>(pkt->size)))
        return rewriteNals(pkt, 4, toSize, prefix);
    std::vector<std::vector<std::uint8_t>> nals = prefix;
    for (auto& nal : splitAnnexB(pkt->data, static_cast<std::size_t>(pkt->size))) {
        if (!nal.empty())
            nals.push_back(std::move(nal));
    }
    std::vector<std::uint8_t> data;
    for (const auto& nal : nals) {
        for (int b = toSize - 1; b >= 0; --b)
            data.push_back(static_cast<std::uint8_t>(nal.size() >> (8 * b)));
        data.insert(data.end(), nal.begin(), nal.end());
    }
    replacePacketData(pkt, data.data(), data.size());
    return true;
}

bool sameAudio(const StreamSignature& a, const StreamSignature& b)
{
    return a.codec == b.codec && a.sampleRate == b.sampleRate && a.channels == b.channels
        && a.channelLayout == b.channelLayout && a.extradata == b.extradata;
}

AVCodecID codecIdOf(const std::string& name)
{
    const AVCodecDescriptor* desc = avcodec_descriptor_get_by_name(name.c_str());
    return desc != nullptr ? desc->id : AV_CODEC_ID_NONE;
}

const StreamSignature* streamOf(const MediaSignature& signature, StreamKind kind)
{
    const auto it = std::find_if(signature.streams.begin(), signature.streams.end(),
                                 [kind](const StreamSignature& s) { return s.kind == kind; });
    return it == signature.streams.end() ? nullptr : &*it;
}

// Una parte di un tratto: copiata o ricodificata.
struct Piece
{
    TimeRange range;
    bool reencode = false;
    // Il tratto continua nella parte dopo (dallo stesso file): l'audio è continuo, quindi il blocco a cavallo della
    // giunzione va in questa parte (non si perde) e la parte dopo parte esattamente alla fine di questa.
    bool continues = false;
};

// Taglio preciso di un tratto copiato (vedi exportClips): ricodificati i bordi fra IN e il primo fotogramma chiave e, con
// i B-frame, fra l'ultimo fotogramma chiave e OUT; copiato il resto. Tutto ricodificato se dentro non c'è un fotogramma
// chiave da cui copiare.
std::vector<Piece> splitForPreciseCut(const TimeRange& range, const std::vector<double>& keyframes, bool bFrames)
{
    constexpr double kOnKeyframe = 1e-3;  // entro un millesimo: è lo stesso fotogramma
    const auto first = std::lower_bound(keyframes.begin(), keyframes.end(), range.start - kOnKeyframe);
    if (first == keyframes.end() || *first >= range.end - kOnKeyframe)
        return {{range, true}};
    const double copyStart = std::max(*first, range.start);
    double copyEnd = range.end;
    if (bFrames) {
        // OUT su un fotogramma chiave: fino lì si copia. Altrimenti si copia fino all'ultimo prima di OUT.
        const auto atEnd = std::lower_bound(keyframes.begin(), keyframes.end(), range.end - kOnKeyframe);
        const bool outOnKeyframe = atEnd != keyframes.end() && *atEnd <= range.end + kOnKeyframe;
        if (!outOnKeyframe) {
            const double last = *std::prev(atEnd);
            if (last <= copyStart + kOnKeyframe)
                return {{range, true}};
            copyEnd = last;
        }
    }
    std::vector<Piece> pieces;
    if (copyStart > range.start + kOnKeyframe)
        pieces.push_back({{range.start, copyStart}, true});
    pieces.push_back({{copyStart, copyEnd}, false});
    if (copyEnd < range.end - kOnKeyframe)
        pieces.push_back({{copyEnd, range.end}, true});
    for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
        pieces[i].continues = true;
    return pieces;
}

// Che cosa succede a ogni file della lista all'esportazione.
struct PlanAnalysis
{
    std::vector<MediaSignature> signatures;     // per file della lista (vuoto: un solo file, si copia com'è)
    std::vector<bool> reencode;                 // per file
    std::vector<bool> copyAudio;                // per file: l'audio si copia (uguale a quello del formato)
    std::vector<std::vector<std::vector<Piece>>> pieces;  // per file e per tratto: le parti, copiate o ricodificate
    std::string impreciseCuts;                  // vedi ExportPreview
    ProjectFormat reference;                    // il formato da rispettare
    bool parametersInStream = false;            // vedi ExportResult
    std::vector<ParameterSets> clipParameters;  // per file copiato, se parametersInStream
    std::string problem;                        // perché non si può esportare; vuoto = si può
};

PlanAnalysis analyzePlan(const std::vector<ClipRanges>& clips, const ProjectFormat* format, bool replaced, bool reencodeAll)
{
    PlanAnalysis plan;
    plan.reencode.assign(clips.size(), reencodeAll);
    plan.copyAudio.assign(clips.size(), false);
    const bool checkFormat = format != nullptr && !format->empty();
    // Lo stesso file può tornare più volte (montaggio A, B, A, B): lo si legge una volta sola. Con una traccia
    // sostitutiva l'audio dei file non si copia, quindi conta solo il video.
    std::map<std::string, MediaSignature> read;
    for (const ClipRanges& clip : clips) {
        auto known = read.find(clip.path);
        if (known == read.end())
            known = read.emplace(clip.path, readMediaSignature(clip.path)).first;
        plan.signatures.push_back(known->second);
        if (replaced)
            std::erase_if(plan.signatures.back().streams, [](const StreamSignature& s) { return s.kind == StreamKind::Audio; });
    }
    plan.reference = checkFormat ? *format : formatOf(plan.signatures.front());
    if (replaced)
        std::erase_if(plan.reference.streams, [](const StreamSignature& s) { return s.kind == StreamKind::Audio; });
    std::string differences;  // dei file da ricodificare, per il messaggio se non si può
    for (std::size_t c = 0; c < clips.size(); ++c) {
        const ClipFitResult fit = classifyClip(plan.signatures[c], plan.reference, !replaced);
        if (fit.fit != ClipFit::Reencode)
            continue;
        plan.reencode[c] = true;
        for (const std::string& reason : fit.reasons)
            differences += "\nClip " + std::to_string(c + 1) + " (" + plan.signatures[c].fileName + "): " + reason;
    }

    const bool anyReencoded = std::find(plan.reencode.begin(), plan.reencode.end(), true) != plan.reencode.end();
    const StreamSignature* video = plan.reference.video();
    // Perché non si può ricodificare nel formato del progetto (vuoto = si può): un video H.264 o HEVC con un encoder su
    // questo PC e i parametri in un formato che si sa ripetere nel flusso.
    std::string cannotReencode;
    if (video == nullptr) {
        cannotReencode = "Il formato del progetto (" + plan.reference.sourceName + ") non ha video: la ricodifica per ora "
                         "si fa solo per i video.";
    } else if (video->codec != "h264" && video->codec != "hevc") {
        cannotReencode = "Il formato del progetto (" + plan.reference.sourceName + ") è " + video->codec
                       + ": la ricodifica per ora si fa solo in H.264 e HEVC.";
    } else {
        const VideoCodec codec = video->codec == "hevc" ? VideoCodec::Hevc : VideoCodec::H264;
        if (!preferredVideoEncoder(codec)) {
            cannotReencode = "Per ricodificare in " + codecName(codec) + " serve un encoder, ma su questo PC non ce n'è "
                             "uno funzionante (vedi Aiuto > Informazioni).";
        } else if (!parameterSetsOf(codecIdOf(video->codec), video->extradata)) {
            cannotReencode = "I parametri di codifica del formato del progetto (" + plan.reference.sourceName
                           + ") sono in un formato che non si sa ripetere nel flusso (Annex B).";
        }
    }

    // Le parti di ogni tratto. Nei file copiati, il taglio preciso (bordi ricodificati) se si può; altrimenti IN va al
    // fotogramma chiave prima, come senza ricodifica.
    std::map<std::string, std::vector<double>> keyframesOf;  // letti dal file se il piano non li ha
    bool preciseNeeded = false;
    plan.pieces.resize(clips.size());
    for (std::size_t c = 0; c < clips.size(); ++c) {
        std::vector<double> keyframes = clips[c].keyframes;
        if (keyframes.empty() && !plan.reencode[c]) {
            auto known = keyframesOf.find(clips[c].path);
            if (known == keyframesOf.end())
                known = keyframesOf.emplace(clips[c].path, probeFile(clips[c].path).keyframeTimes).first;
            keyframes = known->second;
        }
        std::sort(keyframes.begin(), keyframes.end());
        const StreamSignature* own = streamOf(plan.signatures[c], StreamKind::Video);
        for (const TimeRange& range : clips[c].ranges) {
            if (plan.reencode[c] || own == nullptr || keyframes.empty()) {
                plan.pieces[c].push_back({{range, plan.reencode[c]}});
                continue;
            }
            std::vector<Piece> pieces = splitForPreciseCut(range, keyframes, own->bFrames);
            const bool precise = std::any_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.reencode; });
            if (precise && !cannotReencode.empty()) {
                // Senza ricodifica: dal fotogramma chiave nello stesso punto o prima di IN.
                pieces = {{{snapToKeyframe(keyframes, range.start), range.end}, false}};
                if (plan.impreciseCuts.empty())
                    plan.impreciseCuts = "Il taglio preciso non è possibile, quindi IN va al fotogramma chiave più vicino prima: "
                                       + cannotReencode;
            }
            preciseNeeded = preciseNeeded || (precise && cannotReencode.empty());
            plan.pieces[c].push_back(std::move(pieces));
        }
    }

    if (anyReencoded || preciseNeeded) {
        if (anyReencoded) {
            plan.problem = cannotReencode;
            if (!plan.problem.empty() && video != nullptr && video->codec != "h264" && video->codec != "hevc")
                plan.problem += " Scegli come formato un file H.264 o HEVC (Montaggio > Impostazioni del progetto) oppure togli i "
                                "pezzi segnati con R.";
        }
        const StreamSignature* audio = plan.reference.audio();
        for (std::size_t c = 0; c < clips.size() && plan.problem.empty(); ++c) {
            if (!plan.reencode[c]) {
                // Copiato (con i bordi ricodificati): l'audio è uguale a quello del formato, quindi si copia.
                const StreamSignature* own = streamOf(plan.signatures[c], StreamKind::Audio);
                plan.copyAudio[c] = audio != nullptr && own != nullptr && sameAudio(*own, *audio);
                continue;
            }
            if (streamOf(plan.signatures[c], StreamKind::Video) == nullptr) {
                plan.problem = "Il file " + plan.signatures[c].fileName + " non ha video: non si può mettere in un "
                               "montaggio video.";
                break;
            }
            if (audio == nullptr)
                continue;
            const StreamSignature* own = streamOf(plan.signatures[c], StreamKind::Audio);
            plan.copyAudio[c] = own != nullptr && sameAudio(*own, *audio);
            if (!plan.copyAudio[c] && audio->codec != "aac")
                plan.problem = "L'audio del formato del progetto è " + audio->codec + ": per ora si ricodifica solo in AAC.";
        }
        plan.parametersInStream = true;
        // Il messaggio dice anche quali file sono diversi e perché.
        if (!plan.problem.empty() && !differences.empty())
            plan.problem = "Questi file non si possono unire senza ricodifica:" + differences + "\n\nE non si possono ricodificare: "
                         + plan.problem;
    } else {
        // Il file creato ha i parametri del primo file: se un altro ne ha di diversi, ognuno ripete i suoi nel flusso.
        const ProjectFormat first = formatOf(plan.signatures.front());
        for (std::size_t c = 1; c < clips.size() && !plan.parametersInStream; ++c)
            plan.parametersInStream = classifyClip(plan.signatures[c], first, !replaced).fit == ClipFit::CopyWithParameters;
    }
    if (plan.parametersInStream && plan.problem.empty()) {
        plan.clipParameters.resize(clips.size());
        for (std::size_t c = 0; c < clips.size(); ++c) {
            if (plan.reencode[c])
                continue;
            const StreamSignature* v = streamOf(plan.signatures[c], StreamKind::Video);
            const auto sets = v != nullptr ? parameterSetsOf(codecIdOf(v->codec), v->extradata) : std::nullopt;
            if (!sets) {
                plan.problem = "Il file " + plan.signatures[c].fileName + " ha parametri di codifica diversi dagli altri, in "
                               "un formato che senza ricodifica non si sa ripetere nel flusso (Annex B).";
                break;
            }
            plan.clipParameters[c] = *sets;
        }
    }
    return plan;
}

bool samePath(const std::string& a, const std::string& b)
{
    std::error_code ec;
    const std::filesystem::path pa = pathFromUtf8(a);
    const std::filesystem::path pb = pathFromUtf8(b);
    if (std::filesystem::exists(pb, ec) && std::filesystem::equivalent(pa, pb, ec))
        return true;
    // Su Windows i nomi dei file non distinguono maiuscole e minuscole.
    return lowercase(utf8FromPath(std::filesystem::weakly_canonical(pa, ec)))
        == lowercase(utf8FromPath(std::filesystem::weakly_canonical(pb, ec)));
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

    replacePacketData(pkt, pkt->data + static_cast<std::ptrdiff_t>(first) * copy.pcmBlockAlign,
                      static_cast<std::size_t>(last - first) * copy.pcmBlockAlign);
    shiftPacket(pkt, -static_cast<std::int64_t>(std::llround(first / rate / tb)));
    pkt->duration = std::llround((last - first) / rate / tb);
    return true;
}

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
        throw MediaError(kOutOfMemory);
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
                throw MediaError(kOutOfMemory);
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
    replacePacketData(pkt, copy.silence.data(), copy.silence.size());
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
    PacketPtr packet;           // riusato per ogni lettura
    bool pending = false;       // `packet` letto ma non ancora scritto
    bool ended = false;
};

// Prova a scrivere un file del formato `format` con il solo flusso `in` e qualche suo pacchetto (in un file
// temporaneo): alcuni contenitori dichiarano di accettare un codec ma poi non riescono a chiudere il file.
bool canMux(const AVOutputFormat* format, const std::string& extension, AVFormatContext* input, int streamIndex)
{
    std::error_code ec;
    static std::atomic<int> counter{0};
    const std::filesystem::path probe = std::filesystem::temp_directory_path(ec)
                                      / ("EditorVideoProvaAudio" + std::to_string(++counter) + extension);
    const std::string probePath = utf8FromPath(probe);
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

// Come replacementAudioProblem, con il file audio già aperto in `ctx` e il suo flusso audio principale `index`
// (-1 se non c'è). Legge qualche pacchetto: prima di usare `ctx` per copiare l'audio bisogna riposizionarsi.
std::string replacementProblem(AVFormatContext* ctx, int index, const std::string& audioPath,
                               const std::string& outputPath)
{
    if (index < 0)
        return "Il file " + fileNameOf(audioPath) + " non contiene audio.";
    const AVOutputFormat* format = av_guess_format(nullptr, outputPath.c_str(), nullptr);
    if (format == nullptr)
        return {};  // lo dirà l'esportazione
    const AVCodecID codec = ctx->streams[index]->codecpar->codec_id;
    const std::string extension = pathFromUtf8(outputPath).extension().string();  // con il punto
    if (avformat_query_codec(format, codec, FF_COMPLIANCE_NORMAL) == 0 || !canMux(format, extension, ctx, index)) {
        return "L'audio di " + fileNameOf(audioPath) + " (" + avcodec_get_name(codec) + ") non può stare in un file "
             + uppercase(extension.empty() ? extension : extension.substr(1))
             + " senza ricodifica. Scegli un audio in un formato adatto (per MP4 e MOV: AAC, M4A o MP3), "
               "oppure monta file MKV, che accettano quasi tutti i formati audio.";
    }
    return {};
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
    return replacementProblem(ctx.get(), findMainStream(ctx.get(), AVMEDIA_TYPE_AUDIO), audioPath, outputPath);
}

ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                         const ExportProgress& progress, MuteMethod muteMethod, const ProjectFormat* format,
                         const ReencodeOptions& reencode)
{
    double totalLength = 0.0;
    int rangeCount = 0;
    for (const ClipRanges& clip : clips) {
        for (const TimeRange& range : clip.ranges) {
            if (!(range.end > range.start + kTimeEpsilon))
                throw MediaError("OUT deve stare dopo IN.");
            totalLength += range.length();
            ++rangeCount;
        }
        if (samePath(clip.path, outputPath))
            throw MediaError("Il file di destinazione deve essere diverso dagli originali.");
    }
    if (rangeCount == 0)
        throw MediaError("Non c'è nessun tratto da esportare.");
    // Traccia sostitutiva: il file audio si apre una volta sola, per il controllo e poi per la copia.
    const bool replaced = audio.replacement.has_value();
    ReplacementSource replacement;
    if (replaced) {
        if (samePath(audio.replacement->path, outputPath))
            throw MediaError("Il file di destinazione deve essere diverso dall'audio sostitutivo.");
        try {
            replacement.ctx = openInput(audio.replacement->path);
        } catch (const std::exception& e) {
            throw MediaError(e.what());
        }
        replacement.stream = findMainStream(replacement.ctx.get(), AVMEDIA_TYPE_AUDIO);
        if (const std::string problem =
                replacementProblem(replacement.ctx.get(), replacement.stream, audio.replacement->path, outputPath);
            !problem.empty())
            throw MediaError(problem);
    }

    // Che cosa succede a ogni file: si copia (con i suoi parametri nel flusso, se diversi) o si ricodifica.
    const PlanAnalysis plan = analyzePlan(clips, format, replaced, reencode.all);
    if (!plan.problem.empty())
        throw ClipsNotJoinable(plan.problem);
    const std::vector<MediaSignature>& signatures = plan.signatures;
    const bool parametersInStream = plan.parametersInStream;
    const std::vector<ParameterSets>& clipParameters = plan.clipParameters;

    InputFile first = openClip(clips.front().path, !replaced);
    const int clipStreams = first.copied;
    AVFormatContext* rawOutput = nullptr;
    if (avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, outputPath.c_str()) < 0 || rawOutput == nullptr)
        throw MediaError("Dall'estensione non si capisce in che formato salvare: " + outputPath);
    OutputPtr output(rawOutput);

    // Flussi del file creato: quelli del primo file, con gli stessi parametri dei codec. Se il primo file si ricodifica,
    // quelli del formato del progetto.
    for (unsigned i = 0; !plan.reencode.front() && i < first.ctx->nb_streams; ++i) {
        if (first.streams[i].output < 0)
            continue;
        const AVStream* in = first.ctx->streams[i];
        const AVCodecParameters* par = in->codecpar;
        if (avformat_query_codec(output->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            throw MediaError("Il formato " + std::string(output->oformat->name) + " non può contenere il codec "
                             + avcodec_get_name(par->codec_id) + ".");
        AVStream* out = avformat_new_stream(output.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, par) < 0)
            throw MediaError(kOutOfMemoryForOutput);
        out->codecpar->codec_tag = codecTagFor(output->oformat, par);
        // H.264 da file con livelli diversi: il file unito dichiara il livello più alto (il resto dei parametri
        // coincide, l'ha verificato checkCompatibility).
        if (par->codec_id == AV_CODEC_ID_H264 && !signatures.empty()) {
            const int index = first.streams[i].output;
            int level = 0;
            for (std::size_t s = 0; s < signatures.size(); ++s) {
                if (!plan.reencode[s] && index < static_cast<int>(signatures[s].streams.size()))
                    level = std::max(level, maxH264Level(parseH264Config(signatures[s].streams[index].extradata)));
            }
            const std::vector<std::uint8_t> current(par->extradata, par->extradata + par->extradata_size);
            if (level > maxH264Level(parseH264Config(current))) {
                const std::vector<std::uint8_t> raised = withH264Level(current, level);
                av_freep(&out->codecpar->extradata);
                out->codecpar->extradata = static_cast<std::uint8_t*>(av_mallocz(raised.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (out->codecpar->extradata == nullptr)
                    throw MediaError(kOutOfMemoryForOutput);
                std::copy(raised.begin(), raised.end(), out->codecpar->extradata);
                out->codecpar->extradata_size = static_cast<int>(raised.size());
                out->codecpar->level = level;
            }
        }
        // Parametri nel flusso: in un MP4 il video lo dichiara (avc3 / hev1 invece di avc1 / hvc1).
        if (parametersInStream && par->codec_type == AVMEDIA_TYPE_VIDEO && output->oformat->codec_tag != nullptr) {
            const unsigned tag = par->codec_id == AV_CODEC_ID_HEVC ? MKTAG('h', 'e', 'v', '1') : MKTAG('a', 'v', 'c', '3');
            if (av_codec_get_id(output->oformat->codec_tag, tag) == par->codec_id)
                out->codecpar->codec_tag = tag;
        }
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);
    }

    if (plan.reencode.front()) {
        for (const StreamSignature& s : plan.reference.streams) {
            AVStream* out = avformat_new_stream(output.get(), nullptr);
            if (out == nullptr)
                throw MediaError(kOutOfMemoryForOutput);
            AVCodecParameters* par = out->codecpar;
            par->codec_id = codecIdOf(s.codec);
            if (avformat_query_codec(output->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) == 0)
                throw MediaError("Il formato " + std::string(output->oformat->name) + " non può contenere il codec " + s.codec + ".");
            if (!s.extradata.empty()) {
                par->extradata = static_cast<std::uint8_t*>(av_mallocz(s.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (par->extradata == nullptr)
                    throw MediaError(kOutOfMemoryForOutput);
                std::copy(s.extradata.begin(), s.extradata.end(), par->extradata);
                par->extradata_size = static_cast<int>(s.extradata.size());
            }
            if (s.kind == StreamKind::Video) {
                par->codec_type = AVMEDIA_TYPE_VIDEO;
                par->width = s.width;
                par->height = s.height;
                par->format = av_get_pix_fmt(s.pixelFormat.c_str());
                par->sample_aspect_ratio = {1, 1};
                const unsigned tag = par->codec_id == AV_CODEC_ID_HEVC ? MKTAG('h', 'e', 'v', '1') : MKTAG('a', 'v', 'c', '3');
                par->codec_tag = output->oformat->codec_tag != nullptr && av_codec_get_id(output->oformat->codec_tag, tag) == par->codec_id
                                     ? tag
                                     : 0;
                out->time_base = {1, 90000};
                out->avg_frame_rate = av_d2q(s.frameRate > 0 ? s.frameRate : 30.0, 1001000);
            } else {
                par->codec_type = AVMEDIA_TYPE_AUDIO;
                par->sample_rate = s.sampleRate;
                av_channel_layout_default(&par->ch_layout, std::max(1, s.channels));
                par->frame_size = par->codec_id == AV_CODEC_ID_AAC ? 1024 : 0;
                out->time_base = {1, std::max(1, s.sampleRate)};
            }
            out->disposition = AV_DISPOSITION_DEFAULT;
        }
    }

    // Traccia sostitutiva: un flusso audio in più, copiato dal file audio (aperto e controllato sopra).
    if (replaced) {
        replacement.packet.reset(av_packet_alloc());
        if (!replacement.packet)
            throw MediaError(kOutOfMemory);
        keepOnlyStream(replacement.ctx.get(), replacement.stream);
        replacement.start = startOffsetSeconds(replacement.ctx.get());
        replacement.offset = audio.replacement->offset;
        const AVStream* in = replacement.ctx->streams[replacement.stream];
        describeAudio(replacement.copy, in->codecpar);
        AVStream* out = avformat_new_stream(output.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
            throw MediaError(kOutOfMemoryForOutput);
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
    // Video e audio dei file (non la traccia sostitutiva) nel file creato, per i tratti ricodificati.
    int outVideo = -1;
    int outAudio = -1;
    for (int i = 0; i < streamCount; ++i) {
        const AVCodecParameters* par = output->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && outVideo < 0)
            outVideo = i;
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO && outAudio < 0 && !(replaced && i == replacement.copy.output))
            outAudio = i;
    }
    // Byte della lunghezza davanti ai NAL nei pacchetti video del file creato (per i parametri nel flusso).
    int outLength = 4;
    if (parametersInStream && outVideo >= 0) {
        const AVCodecParameters* par = output->streams[outVideo]->codecpar;
        if (const auto sets = parameterSetsOf(par->codec_id, std::vector<std::uint8_t>(par->extradata, par->extradata + par->extradata_size)))
            outLength = sets->lengthSize;
    }

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
            throw MediaError(kOutOfMemory);

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
            AVPacket* pkt = replacement.packet.get();
            while (!replacement.ended) {
                if (!replacement.pending) {
                    const int ret = av_read_frame(input, pkt);
                    if (ret == AVERROR_EOF) {
                        replacement.ended = true;
                        break;
                    }
                    if (ret < 0)
                        throw MediaError("Errore durante la lettura dell'audio sostitutivo (" + ffmpegError(ret) + ").");
                    if (pkt->stream_index != replacement.stream || (pkt->pts == AV_NOPTS_VALUE && pkt->dts == AV_NOPTS_VALUE)) {
                        av_packet_unref(pkt);
                        continue;
                    }
                    replacement.pending = true;
                }
                const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                const double sourceTime = ts * tb - replacement.start;
                double time = sourceTime + replacement.offset;  // nel file creato
                if (time >= until)
                    break;
                if (time >= end - kTimeEpsilon) {
                    replacement.ended = true;
                    break;
                }
                // Da qui il pacchetto si scrive o si scarta: in entrambi i casi si libera prima della prossima lettura.
                replacement.pending = false;
                const PacketUnref release{pkt};
                double duration = std::max<std::int64_t>(pkt->duration, 0) * tb;
                if (replacement.copy.pcmBlockAlign > 0) {
                    if (!trimPcm(pkt, replacement.copy, sourceTime, tb, {-replacement.offset, end - replacement.offset}))
                        continue;
                    time = (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts) * tb - replacement.start + replacement.offset;
                    duration = pkt->duration * tb;
                } else if (time < -kTimeEpsilon || time + duration > end + kTimeEpsilon) {
                    continue;  // solo i pacchetti interamente dentro il file creato
                }
                if (!applyMute(pkt, replacement.copy, in->codecpar, time, time + duration, audio.muted, muteMethod, result))
                    continue;
                shiftPacket(pkt, std::llround((replacement.start - replacement.offset) / tb));
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

        // Ricodifica di un tratto. Il video si decodifica e si ricodifica nel formato del progetto: a fotogrammi al
        // secondo costanti, per ogni fotogramma del file creato quello dell'originale che si vede in quel momento (ripetuto
        // o saltato se le frequenze sono diverse, senza deriva anche con 59,94 o una frequenza variabile); misura e
        // formato dei pixel li adatta l'encoder (bande nere se le proporzioni sono diverse). L'audio si copia se è uguale
        // a quello del formato, altrimenti si ricodifica in AAC (silenzio se il file non ha audio). Restituisce dove
        // finiscono, nel file creato, il video e l'audio scritti.
        const auto reencodeRange = [&](InputFile& current, const TimeRange& range, std::size_t c,
                                       bool continues) -> std::pair<double, double> {
            AVFormatContext* input = current.ctx.get();
            const double start = current.start;
            const StreamSignature& format = *plan.reference.video();
            const AVRational fps = av_d2q(format.frameRate > 0 ? format.frameRate : 30.0, 1001000);
            const double frameSeconds = av_q2d(av_inv_q(fps));
            const int frames = std::max(1, static_cast<int>(std::llround(range.length() / frameSeconds)));
            const std::int64_t base = std::llround(outputPosition / frameSeconds);
            AVStream* outV = output->streams[outVideo];

            // Un encoder per tratto: alla fine si svuota, così i suoi pacchetti non si mescolano con quelli dopo.
            VideoEncoderSettings settings;
            settings.codec = format.codec == "hevc" ? VideoCodec::Hevc : VideoCodec::H264;
            settings.width = format.width;
            settings.height = format.height;
            settings.frameRate = {fps.num, fps.den};
            settings.quality = reencode.quality;
            VideoEncoder encoder(settings);
            ParameterSets encoderParameters;
            {
                AVCodecParameters* par = avcodec_parameters_alloc();
                encoder.copyParameters(par);
                const auto sets = parameterNalsOf(par->codec_id, std::vector<std::uint8_t>(par->extradata, par->extradata + par->extradata_size));
                avcodec_parameters_free(&par);
                if (!sets)
                    throw MediaError("L'encoder " + encoder.info().ffmpegName + " non ha dato i parametri di codifica.");
                encoderParameters = *sets;
            }
            const Rational encoderTb = encoder.timeBase();
            const PacketSink videoSink = [&](AVPacket* p) {
                const bool key = (p->flags & AV_PKT_FLAG_KEY) != 0;
                if (!encodedToLengthPrefixed(p, outLength, key ? encoderParameters.nals : std::vector<std::vector<std::uint8_t>>{}))
                    throw MediaError("Un pacchetto dell'encoder non si legge.");
                if (p->duration <= 0)
                    p->duration = 1;
                av_packet_rescale_ts(p, {encoderTb.num, encoderTb.den}, outV->time_base);
                keepDtsIncreasing(p, outVideo);
                const double t = p->pts * av_q2d(outV->time_base);
                firstTime = std::min(firstTime, t);
                lastEnd = std::max(lastEnd, t + frameSeconds);
                pumpReplacement(std::min(t + 0.3, totalLength - 0.1), totalLength);
                p->stream_index = outVideo;
                p->pos = -1;
                if (const int ret = av_interleaved_write_frame(output.get(), p); ret < 0)
                    throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
            };

            // Decoder del video (e dell'audio, se si ricodifica).
            const auto openDecoder = [&](int index) {
                const AVStream* st = input->streams[index];
                const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
                if (codec == nullptr)
                    throw ClipsNotJoinable("Il file " + clips[c].path + " è in " + avcodec_get_name(st->codecpar->codec_id)
                                           + ": questa versione del programma non sa decodificarlo per ricodificarlo.");
                CodecContextPtr ctx(avcodec_alloc_context3(codec));
                avcodec_parameters_to_context(ctx.get(), st->codecpar);
                ctx->pkt_timebase = st->time_base;
                ctx->thread_count = 0;
                if (const int ret = avcodec_open2(ctx.get(), codec, nullptr); ret < 0)
                    throw MediaError("Il decoder di " + clips[c].path + " non si apre: " + ffmpegError(ret));
                return ctx;
            };
            const int videoIndex = findMainStream(input, AVMEDIA_TYPE_VIDEO);
            if (videoIndex < 0)
                throw ClipsNotJoinable("Il file " + clips[c].path + " non ha video.");
            const AVStream* inV = input->streams[videoIndex];
            CodecContextPtr videoDecoder = openDecoder(videoIndex);
            const int audioIndex = outAudio >= 0 ? findMainStream(input, AVMEDIA_TYPE_AUDIO) : -1;
            const bool copyAudio = audioIndex >= 0 && plan.copyAudio[c];
            CodecContextPtr audioDecoder = audioIndex >= 0 && !copyAudio ? openDecoder(audioIndex) : nullptr;
            AVStream* outA = outAudio >= 0 ? output->streams[outAudio] : nullptr;
            std::unique_ptr<AudioEncoder> audioEncoder;
            StreamCopy audioOut;  // il flusso audio del file creato (per il silenzio nei tratti senza audio)
            if (outA != nullptr) {
                describeAudio(audioOut, outA->codecpar);
                audioOut.output = outAudio;
                if (!copyAudio)
                    audioEncoder = std::make_unique<AudioEncoder>(AudioEncoderSettings{outA->codecpar->sample_rate,
                                                                                       outA->codecpar->ch_layout.nb_channels});
            }
            std::vector<AVDiscard> discarded(input->nb_streams);
            for (unsigned i = 0; i < input->nb_streams; ++i) {
                discarded[i] = input->streams[i]->discard;
                input->streams[i]->discard = static_cast<int>(i) == videoIndex || static_cast<int>(i) == audioIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
            }

            // Dal fotogramma chiave all'inizio del tratto (o quello subito prima).
            const auto ts = std::llround((range.start + start) / av_q2d(inV->time_base));
            if (av_seek_frame(input, videoIndex, ts, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(input, videoIndex, 0, AVSEEK_FLAG_BACKWARD);

            // --- Video: fotogramma `k` del tratto al tempo range.start + k / fps.
            FramePtr held;  // l'ultimo fotogramma decodificato non oltre il tempo del prossimo fotogramma da scrivere
            int k = 0;
            bool videoDone = false;
            const double half = frameSeconds / 2;
            const auto emit = [&](const AVFrame* frame) {
                FramePtr copy(av_frame_clone(frame));
                if (!copy)
                    throw MediaError(kOutOfMemory);
                copy->pts = base + k;
                encoder.encode(copy.get(), k == 0, videoSink);
                ++k;
                if (progress && !progress(std::clamp((doneLength + k * frameSeconds) / totalLength, 0.0, 1.0)))
                    throw ExportCancelled();
            };
            const auto onVideoFrame = [&](AVFrame* frame) {
                if (videoDone)
                    return;
                const std::int64_t when = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                if (when == AV_NOPTS_VALUE)
                    return;
                const double t = when * av_q2d(inV->time_base) - start;
                while (k < frames && t > range.start + k * frameSeconds + half)
                    emit(held ? held.get() : frame);
                if (k >= frames) {
                    videoDone = true;
                    return;
                }
                held.reset(av_frame_clone(frame));
            };

            // --- Audio: copiato (come nei tratti copiati) o ricodificato, a partire da outputPosition.
            bool audioDone = audioIndex < 0;
            double audioEnd = outputPosition;
            const int outRate = outA != nullptr ? std::max(1, outA->codecpar->sample_rate) : 1;
            const PacketSink audioSink = [&](AVPacket* p) {
                if (p->pts < 0)
                    return;  // il ritardo iniziale dell'encoder (qualche millesimo): non si sente
                const double time = outputPosition + static_cast<double>(p->pts) / outRate;
                // L'ultimo blocco può dichiarare meno campioni, ma il decoder ne restituisce comunque uno intero.
                const double duration = std::max<std::int64_t>({p->duration, 0, outA->codecpar->frame_size}) / static_cast<double>(outRate);
                const double seqStart = doneLength + (time - outputPosition);
                if (!applyMute(p, audioOut, outA->codecpar, seqStart, seqStart + duration, audio.muted, muteMethod, result))
                    return;
                const std::int64_t shift = std::llround(outputPosition * outRate);
                p->pts += shift;
                if (p->dts != AV_NOPTS_VALUE)
                    p->dts += shift;
                av_packet_rescale_ts(p, {1, outRate}, outA->time_base);
                keepDtsIncreasing(p, outAudio);
                firstTime = std::min(firstTime, time);
                lastEnd = std::max(lastEnd, time + duration);
                audioEnd = std::max(audioEnd, time + duration);
                p->stream_index = outAudio;
                p->pos = -1;
                if (const int ret = av_interleaved_write_frame(output.get(), p); ret < 0)
                    throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
            };
            const auto onAudioFrame = [&](AVFrame* frame) {
                if (audioDone)
                    return;
                const AVStream* in = input->streams[audioIndex];
                const std::int64_t when = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                if (when == AV_NOPTS_VALUE || frame->sample_rate <= 0)
                    return;
                const double t0 = when * av_q2d(in->time_base) - start;
                if (t0 >= range.end) {
                    audioDone = true;
                    return;
                }
                // Solo i campioni dentro il tratto.
                const int n = frame->nb_samples;
                const int skip = std::clamp(static_cast<int>(std::llround((range.start - t0) * frame->sample_rate)), 0, n);
                const int end = std::clamp(static_cast<int>(std::llround((range.end - t0) * frame->sample_rate)), 0, n);
                if (end <= skip)
                    return;
                FramePtr part(av_frame_alloc());
                part->format = frame->format;
                part->sample_rate = frame->sample_rate;
                part->nb_samples = end - skip;
                av_channel_layout_copy(&part->ch_layout, &frame->ch_layout);
                if (av_frame_get_buffer(part.get(), 0) < 0)
                    throw MediaError(kOutOfMemory);
                av_samples_copy(part->extended_data, frame->extended_data, 0, skip, end - skip, frame->ch_layout.nb_channels,
                                static_cast<AVSampleFormat>(frame->format));
                audioEncoder->encode(part.get(), audioSink);
            };
            // Audio copiato: come nei tratti copiati (tutti i tempi spostati dello stesso valore del video).
            const auto onCopiedAudio = [&](AVPacket* pkt) {
                StreamCopy& copy = current.streams[audioIndex];
                const AVStream* in = input->streams[audioIndex];
                const double tb = av_q2d(in->time_base);
                const std::int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                if (ts == AV_NOPTS_VALUE)
                    return;
                const double time = ts * tb - start;
                const double decodeTime = (pkt->dts != AV_NOPTS_VALUE ? pkt->dts : ts) * tb - start;
                if (decodeTime >= range.end + kReadPastOut) {
                    audioDone = true;
                    return;
                }
                if (copy.pcmBlockAlign > 0) {
                    if (!trimPcm(pkt, copy, time, tb, range))
                        return;
                } else {
                    const double end = time + std::max<std::int64_t>(pkt->duration, 0) * tb;
                    if (time < range.start - kTimeEpsilon || (continues ? time >= range.end - kTimeEpsilon : end > range.end + kTimeEpsilon))
                        return;
                }
                const std::int64_t kept = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                const double seqStart = doneLength + (kept * tb - start - range.start);
                double length = std::max<std::int64_t>(pkt->duration, 0) * tb;
                if (length <= 0)
                    length = copy.audioFrameSeconds;
                if (!applyMute(pkt, copy, in->codecpar, seqStart, seqStart + length, audio.muted, muteMethod, result))
                    return;
                shiftPacket(pkt, std::llround((range.start + start - outputPosition) / tb));
                av_packet_rescale_ts(pkt, in->time_base, outA->time_base);
                keepDtsIncreasing(pkt, outAudio);
                const double t = pkt->pts * av_q2d(outA->time_base);
                const double duration = std::max(pkt->duration * av_q2d(outA->time_base), copy.audioFrameSeconds);
                firstTime = std::min(firstTime, t);
                lastEnd = std::max(lastEnd, t + duration);
                audioEnd = std::max(audioEnd, t + duration);
                pkt->stream_index = outAudio;
                pkt->pos = -1;
                if (const int ret = av_interleaved_write_frame(output.get(), pkt); ret < 0)
                    throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
            };

            PacketPtr pkt(av_packet_alloc());
            FramePtr frame(av_frame_alloc());
            if (!pkt || !frame)
                throw MediaError(kOutOfMemory);
            const auto drain = [&](AVCodecContext* decoder, const auto& onFrame) {
                while (avcodec_receive_frame(decoder, frame.get()) >= 0) {
                    onFrame(frame.get());
                    av_frame_unref(frame.get());
                }
            };
            while (!(videoDone && audioDone)) {
                const int ret = av_read_frame(input, pkt.get());
                if (ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    throw MediaError("Errore durante la lettura del file originale (" + ffmpegError(ret) + ").");
                const PacketUnref release{pkt.get()};
                if (pkt->stream_index == videoIndex && !videoDone) {
                    if (avcodec_send_packet(videoDecoder.get(), pkt.get()) >= 0)  // un pacchetto rovinato si salta
                        drain(videoDecoder.get(), onVideoFrame);
                } else if (pkt->stream_index == audioIndex && !audioDone) {
                    if (copyAudio) {
                        onCopiedAudio(pkt.get());
                    } else if (avcodec_send_packet(audioDecoder.get(), pkt.get()) >= 0) {
                        drain(audioDecoder.get(), onAudioFrame);
                    }
                }
            }
            // Fine del file: quello che resta nei decoder, poi l'ultimo fotogramma ripetuto fino alla fine del tratto.
            if (!videoDone) {
                avcodec_send_packet(videoDecoder.get(), nullptr);
                drain(videoDecoder.get(), onVideoFrame);
            }
            if (audioDecoder && !audioDone) {
                avcodec_send_packet(audioDecoder.get(), nullptr);
                drain(audioDecoder.get(), onAudioFrame);
            }
            if (k == 0 && !held)
                throw MediaError("Nel tratto da ricodificare di " + clips[c].path + " non c'è nessun fotogramma.");
            while (k < frames)
                emit(held.get());
            encoder.finish(videoSink);
            if (audioEncoder) {
                if (audioIndex < 0) {
                    // Il file non ha audio: silenzio per tutto il tratto.
                    const AVCodecParameters* par = outA->codecpar;
                    std::int64_t remaining = std::llround(range.length() * outRate);
                    while (remaining > 0) {
                        FramePtr silence(av_frame_alloc());
                        silence->format = AV_SAMPLE_FMT_FLTP;
                        silence->sample_rate = outRate;
                        silence->nb_samples = static_cast<int>(std::min<std::int64_t>(remaining, 1024));
                        av_channel_layout_default(&silence->ch_layout, std::max(1, par->ch_layout.nb_channels));
                        if (av_frame_get_buffer(silence.get(), 0) < 0)
                            throw MediaError(kOutOfMemory);
                        av_samples_set_silence(silence->extended_data, 0, silence->nb_samples, silence->ch_layout.nb_channels,
                                               AV_SAMPLE_FMT_FLTP);
                        audioEncoder->encode(silence.get(), audioSink);
                        remaining -= silence->nb_samples;
                    }
                }
                audioEncoder->finish(audioSink);
            }
            for (unsigned i = 0; i < input->nb_streams; ++i)
                input->streams[i]->discard = discarded[i];  // i flussi copiati nelle parti dopo
            return {outputPosition + frames * frameSeconds, audioEnd};
        };

        for (std::size_t c = 0; c < clips.size(); ++c) {
            InputFile current = c == 0 ? std::move(first) : openClip(clips[c].path, !replaced);
            if (!plan.reencode[c] && current.copied != clipStreams && !plan.reencode.front())
                throw MediaError("Il file " + clips[c].path + " ha un numero di flussi diverso dal primo.");
            AVFormatContext* input = current.ctx.get();
            const double start = current.start;

            for (std::size_t r = 0; r < clips[c].ranges.size(); ++r) {
              for (const Piece& piece : plan.pieces[c][r]) {
                const TimeRange& range = piece.range;
                if (piece.reencode) {
                    const auto [videoEnd, audioEnd] = reencodeRange(current, range, c, piece.continues);
                    if (plan.reencode[c])
                        ++result.reencodedSegments;
                    else
                        ++result.preciseCuts;
                    // Dentro lo stesso tratto l'audio continua: la parte dopo riparte esattamente da qui.
                    outputPosition = std::max({outputPosition + range.length(), videoEnd, piece.continues ? 0.0 : audioEnd});
                    doneLength += range.length();
                    continue;
                }
                // Tutti i tempi del tratto vengono spostati dello stesso valore, per video e audio:
                // tempo nel file creato = tempo nell'originale - range.start + outputPosition.
                const double shiftSeconds = range.start + start - outputPosition;
                double videoEnd = outputPosition;  // fine dell'ultimo fotogramma scritto di questo tratto
                double audioEnd = outputPosition;  // fine dell'audio compresso come uscirà dal decoder

                const auto write = [&](AVPacket* pkt, StreamCopy& copy, const AVStream* in) {
                    // Parametri nel flusso: davanti a ogni fotogramma chiave quelli di questo file (con la lunghezza dei
                    // NAL del file creato, se diversa).
                    if (parametersInStream && copy.video) {
                        const ParameterSets& mine = clipParameters[c];
                        const int target = outLength;
                        const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                        if ((key || mine.lengthSize != target)
                            && !rewriteNals(pkt, mine.lengthSize, target,
                                            key ? mine.nals : std::vector<std::vector<std::uint8_t>>{}))
                            throw MediaError("Un fotogramma di " + clips[c].path + " non si legge: impossibile copiarlo.");
                    }
                    shiftPacket(pkt, std::llround(shiftSeconds / av_q2d(in->time_base)));
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
                            if ((packet->flags & AV_PKT_FLAG_KEY) == 0 || time < range.start - kTimeEpsilon) {
                                av_packet_unref(packet.get());
                                continue;
                            }
                            copy.started = true;
                        }
                        if (time < range.start - kTimeEpsilon) {
                            // B-frame che precedono il fotogramma chiave (GOP aperto): senza il GOP prima non si
                            // decodificano.
                            av_packet_unref(packet.get());
                            continue;
                        }
                        if (time >= range.end - kTimeEpsilon) {
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
                            // si sovrappone (al più un pacchetto, ~20 ms, di silenzio a ogni giunzione). Dove il tratto
                            // continua nella parte dopo (taglio preciso) l'audio è continuo: il pacchetto a cavallo resta qui.
                            const double end = time + std::max<std::int64_t>(packet->duration, 0) * tb;
                            if (time < range.start - kTimeEpsilon
                                || (piece.continues ? time >= range.end - kTimeEpsilon : end > range.end + kTimeEpsilon)) {
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
                outputPosition = std::max({outputPosition + range.length(), videoEnd, piece.continues ? 0.0 : audioEnd});
                doneLength += range.length();
              }
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
        result.parametersInStream = parametersInStream;
        return result;
    } catch (...) {
        removePartial();
        throw;
    }
}

ExportPreview previewExport(const std::vector<ClipRanges>& clips, const AudioEdit& audio, const ProjectFormat* format,
                            const ReencodeOptions& reencode)
{
    ExportPreview preview;
    if (clips.empty())
        return preview;
    try {
        const PlanAnalysis plan = analyzePlan(clips, format, audio.replacement.has_value(), reencode.all);
        preview.problem = plan.problem;
        preview.impreciseCuts = plan.impreciseCuts;
        for (std::size_t c = 0; c < clips.size(); ++c) {
            for (std::size_t r = 0; r < clips[c].ranges.size(); ++r) {
                if (plan.reencode[c]) {
                    ++preview.reencodedSegments;
                    preview.reencodedSeconds += clips[c].ranges[r].length();
                    preview.outputSeconds += clips[c].ranges[r].length();
                    continue;
                }
                ++preview.copiedSegments;
                for (const Piece& piece : plan.pieces[c][r]) {
                    preview.outputSeconds += piece.range.length();
                    if (piece.reencode) {
                        ++preview.preciseCuts;
                        preview.preciseSeconds += piece.range.length();
                    } else {
                        preview.copiedSeconds += piece.range.length();
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        preview.problem = e.what();
    }
    return preview;
}

ExportResult exportSegments(const std::string& inputPath, const std::string& outputPath,
                            const std::vector<TimeRange>& ranges, const ExportProgress& progress)
{
    return exportClips({{inputPath, ranges}}, outputPath, AudioEdit{}, progress);
}

}  // namespace editor::core
