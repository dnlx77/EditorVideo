#pragma once

// Parti interne dell'esportazione dei tratti, condivise da SegmentExport.cpp e SegmentExportJob.cpp.

#include "EditorCore/SegmentExport.h"

#include "EditorCore/Compatibility.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/MixExport.h"
#include "EditorCore/NalUnits.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/TextFrame.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

namespace editor::core {

using namespace detail;

namespace segment {

inline constexpr const char* kOutOfMemory = "Memoria insufficiente per l'esportazione.";
inline constexpr const char* kOutOfMemoryForOutput = "Memoria insufficiente per preparare il file di destinazione.";

// Dopo la fine di un tratto si legge ancora un po': in un file i pacchetti dei vari flussi sono mescolati e,
// con i B-frame, un fotogramma prima della fine può arrivare dopo uno successivo.
inline constexpr double kReadPastOut = 1.0;

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

inline std::string uppercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// Sostituisce il contenuto del pacchetto con `size` byte da `data` (che possono stare dentro il pacchetto stesso),
// tenendo tempi, durata, flag e flusso.
inline void replacePacketData(AVPacket* pkt, const std::uint8_t* data, std::size_t size)
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
inline void shiftPacket(AVPacket* pkt, std::int64_t shift)
{
    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= shift;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts -= shift;
}

using nal::ParameterSets;

// Dai "dati extra" in formato avcC / hvcC (MP4, MKV). std::nullopt se sono in un altro formato (es. Annex B).
inline std::optional<ParameterSets> parameterSetsOf(AVCodecID codec, const std::vector<std::uint8_t>& extra)
{
    if (codec == AV_CODEC_ID_H264)
        return nal::readAvcC(extra);
    if (codec == AV_CODEC_ID_HEVC)
        return nal::readHvcC(extra);
    return std::nullopt;
}

// Riscrive il pacchetto (NAL con la lunghezza davanti, su `fromSize` byte) con le lunghezze su `toSize` byte e, davanti,
// i NAL di `prefix`. false se il pacchetto non si capisce.
inline bool rewriteNals(AVPacket* pkt, int fromSize, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix)
{
    std::vector<std::uint8_t> data;
    data.reserve(static_cast<std::size_t>(pkt->size) + 256);
    for (const auto& unit : prefix)
        nal::appendLengthPrefixed(data, unit.data(), unit.size(), toSize);
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
        nal::appendLengthPrefixed(data, pkt->data + pos, length, toSize);
        pos += length;
    }
    replacePacketData(pkt, data.data(), data.size());
    return true;
}

// Parametri di codifica (SPS/PPS, per HEVC anche VPS) dai "dati extra", in avcC/hvcC o in Annex B (come li danno
// gli encoder).
inline std::optional<ParameterSets> parameterNalsOf(AVCodecID codec, const std::vector<std::uint8_t>& extra)
{
    if (!extra.empty() && extra[0] == 1)
        return parameterSetsOf(codec, extra);
    ParameterSets sets;
    for (auto& unit : nal::splitAnnexB(extra.data(), extra.size())) {
        const int type = codec == AV_CODEC_ID_HEVC ? (unit[0] >> 1) & 0x3f : unit[0] & 0x1f;
        const bool parameter = codec == AV_CODEC_ID_HEVC ? (type == 32 || type == 33 || type == 34) : (type == 7 || type == 8);
        if (parameter)
            sets.nals.push_back(std::move(unit));
    }
    if (sets.nals.empty())
        return std::nullopt;
    return sets;
}

// Pacchetto dell'encoder (Annex B, o già con le lunghezze su 4 byte) con le lunghezze su `toSize` byte e, davanti, i
// NAL di `prefix`.
inline bool encodedToLengthPrefixed(AVPacket* pkt, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix)
{
    if (!nal::startsWithStartCode(pkt->data, static_cast<std::size_t>(pkt->size)))
        return rewriteNals(pkt, 4, toSize, prefix);
    std::vector<std::uint8_t> data;
    for (const auto& unit : prefix)
        nal::appendLengthPrefixed(data, unit.data(), unit.size(), toSize);
    for (const auto& unit : nal::splitAnnexB(pkt->data, static_cast<std::size_t>(pkt->size)))
        nal::appendLengthPrefixed(data, unit.data(), unit.size(), toSize);
    replacePacketData(pkt, data.data(), data.size());
    return true;
}

inline AVCodecID codecIdOf(const std::string& name)
{
    const AVCodecDescriptor* desc = avcodec_descriptor_get_by_name(name.c_str());
    return desc != nullptr ? desc->id : AV_CODEC_ID_NONE;
}

// Una parte di un tratto: copiata o ricodificata.
struct Piece
{
    TimeRange range;
    bool reencode = false;
    // Il tratto continua nella parte dopo (dallo stesso file): l'audio è continuo, quindi il blocco a cavallo della
    // giunzione va in questa parte (non si perde) e la parte dopo parte esattamente alla fine di questa.
    bool continues = false;
    // Ricodificata perché sotto una scritta (vedi ReencodeOptions::texts).
    bool text = false;
};

// Taglio preciso di un tratto copiato (vedi exportClips): ricodificati i bordi fra IN e il primo fotogramma chiave e, con
// i B-frame, fra l'ultimo fotogramma chiave e OUT; copiato il resto. Tutto ricodificato se dentro non c'è un fotogramma
// chiave da cui copiare.
inline std::vector<Piece> splitForPreciseCut(const TimeRange& range, const std::vector<double>& keyframes, bool bFrames)
{
    const auto first = std::lower_bound(keyframes.begin(), keyframes.end(), range.start - kTimeEpsilon);
    if (first == keyframes.end() || *first >= range.end - kTimeEpsilon)
        return {{range, true}};
    const double copyStart = std::max(*first, range.start);
    double copyEnd = range.end;
    if (bFrames) {
        // OUT su un fotogramma chiave: fino lì si copia. Altrimenti si copia fino all'ultimo prima di OUT.
        const auto atEnd = std::lower_bound(keyframes.begin(), keyframes.end(), range.end - kTimeEpsilon);
        const bool outOnKeyframe = atEnd != keyframes.end() && *atEnd <= range.end + kTimeEpsilon;
        if (!outOnKeyframe) {
            const double last = *std::prev(atEnd);
            if (last <= copyStart + kTimeEpsilon)
                return {{range, true}};
            copyEnd = last;
        }
    }
    std::vector<Piece> pieces;
    if (copyStart > range.start + kTimeEpsilon)
        pieces.push_back({{range.start, copyStart}, true});
    pieces.push_back({{copyStart, copyEnd}, false});
    if (copyEnd < range.end - kTimeEpsilon)
        pieces.push_back({{copyEnd, range.end}, true});
    for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
        pieces[i].continues = true;
    return pieces;
}

// I momenti del montaggio (tempi della sequenza) in cui si vede almeno una scritta, in ordine e senza sovrapposizioni.
inline std::vector<TimeRange> textCoverage(const TextTracks* texts)
{
    std::vector<TimeRange> spans;
    if (texts == nullptr)
        return spans;
    for (const TextLayer& layer : texts->layers()) {
        for (const TextClip& clip : layer.clips) {
            if (!clip.text.empty() && clip.length > kTimeEpsilon)
                spans.push_back({clip.position, clip.position + clip.length});
        }
    }
    std::sort(spans.begin(), spans.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& span : spans) {
        if (!merged.empty() && span.start <= merged.back().end + kTimeEpsilon)
            merged.back().end = std::max(merged.back().end, span.end);
        else
            merged.push_back(span);
    }
    return merged;
}

// Un tratto del file (`range`, che nella sequenza parte da `sequenceStart`) diviso nelle parti sotto una scritta (true) e
// nelle altre (false), in ordine. Una sola parte false se non ci sono scritte.
inline std::vector<std::pair<TimeRange, bool>> splitByText(const TimeRange& range, double sequenceStart,
                                                           const std::vector<TimeRange>& coverage)
{
    std::vector<std::pair<TimeRange, bool>> parts;
    double at = range.start;
    const double offset = range.start - sequenceStart;  // tempo del file meno tempo della sequenza
    for (const TimeRange& span : coverage) {
        const double from = std::max(span.start + offset, at);
        const double to = std::min(span.end + offset, range.end);
        if (to - from <= kTimeEpsilon)
            continue;
        if (from > at + kTimeEpsilon)
            parts.push_back({{at, from}, false});
        parts.push_back({{from, to}, true});
        at = to;
    }
    if (parts.empty())
        return {{range, false}};
    if (range.end > at + kTimeEpsilon)
        parts.push_back({{at, range.end}, false});
    else
        parts.back().first.end = range.end;
    return parts;
}

// Che cosa succede a ogni file della lista all'esportazione.
struct PlanAnalysis
{
    std::vector<MediaSignature> signatures;     // per file della lista (vuoto: un solo file, si copia com'è)
    std::vector<bool> reencode;                 // per file
    std::vector<bool> copyAudio;                // per file: l'audio si copia (uguale a quello del formato)
    std::vector<std::vector<std::vector<Piece>>> pieces;  // per file e per tratto: le parti, copiate o ricodificate
    std::string impreciseCuts;                  // vedi ExportPreview
    bool blackGaps = false;                     // gli spazi vuoti diventano nero e silenzio
    std::string closedGaps;                     // vedi ExportPreview
    ProjectFormat reference;                    // il formato da rispettare
    bool parametersInStream = false;            // vedi ExportResult
    std::vector<ParameterSets> clipParameters;  // per file copiato, se parametersInStream
    std::string problem;                        // perché non si può esportare; vuoto = si può
};

// Spazio vuoto prima del tratto `r` di `clip` (0 se il piano non ne ha).
inline double gapBefore(const ClipRanges& clip, std::size_t r)
{
    return r < clip.gapsBefore.size() ? std::max(0.0, clip.gapsBefore[r]) : 0.0;
}

inline PlanAnalysis analyzePlan(const std::vector<ClipRanges>& clips, const ProjectFormat* format, bool replaced, bool reencodeAll,
                         bool closeGaps, const TextTracks* texts = nullptr)
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
            known = read.emplace(clip.path, clip.signature ? *clip.signature : readMediaSignature(clip.path)).first;
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
    const std::vector<TimeRange> coverage = textCoverage(texts);
    bool textNeeded = false;
    double sequence = 0.0;  // dove cade il tratto nel montaggio (spazi vuoti compresi)
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
        const StreamSignature* own = findStream(plan.signatures[c].streams, StreamKind::Video);
        for (std::size_t r = 0; r < clips[c].ranges.size(); ++r) {
            const TimeRange& range = clips[c].ranges[r];
            sequence += gapBefore(clips[c], r);
            const double sequenceStart = sequence;
            sequence += range.length();
            if (plan.reencode[c] || own == nullptr || keyframes.empty()) {
                plan.pieces[c].push_back({{range, plan.reencode[c]}});
                continue;
            }
            // Le parti sotto una scritta si ricodificano (la scritta si disegna sul fotogramma); le altre come sempre.
            std::vector<Piece> pieces;
            bool anyText = false;
            for (const auto& [part, covered] : splitByText(range, sequenceStart, coverage)) {
                if (covered) {
                    anyText = true;
                    Piece piece{part, true};
                    piece.text = true;
                    pieces.push_back(piece);
                    continue;
                }
                std::vector<Piece> own_pieces = splitForPreciseCut(part, keyframes, own->bFrames);
                pieces.insert(pieces.end(), own_pieces.begin(), own_pieces.end());
            }
            for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
                pieces[i].continues = true;
            if (anyText)
                textNeeded = true;
            const bool precise = std::any_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.reencode && !p.text; });
            if ((precise || anyText) && !cannotReencode.empty()) {
                // Senza ricodifica: dal fotogramma chiave nello stesso punto o prima di IN (se le scritte lo richiedono, il
                // piano ha un problema, vedi sotto).
                pieces = {{{snapToKeyframe(keyframes, range.start), range.end}, false}};
                if (precise && !anyText && plan.impreciseCuts.empty())
                    plan.impreciseCuts = "Il taglio preciso non è possibile, quindi IN va al fotogramma chiave più vicino prima: "
                                       + cannotReencode;
            }
            preciseNeeded = preciseNeeded || ((precise || anyText) && cannotReencode.empty());
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
                const StreamSignature* own = findStream(plan.signatures[c].streams, StreamKind::Audio);
                plan.copyAudio[c] = audio != nullptr && own != nullptr && sameAudioFormat(*own, *audio);
                continue;
            }
            if (findStream(plan.signatures[c].streams, StreamKind::Video) == nullptr) {
                plan.problem = "Il file " + plan.signatures[c].fileName + " non ha video: non si può mettere in un "
                               "montaggio video.";
                break;
            }
            if (audio == nullptr)
                continue;
            const StreamSignature* own = findStream(plan.signatures[c].streams, StreamKind::Audio);
            plan.copyAudio[c] = own != nullptr && sameAudioFormat(*own, *audio);
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
    if (textNeeded && !cannotReencode.empty() && plan.problem.empty())
        plan.problem = "Le scritte si disegnano sul video, quindi i tratti sotto le scritte vanno ricodificati, ma non si può: "
                     + cannotReencode;
    // Spazi vuoti: nero e silenzio nel formato del progetto (con l'encoder, come la ricodifica), se si può.
    bool anyGap = false;
    for (const ClipRanges& clip : clips) {
        for (std::size_t r = 0; r < clip.ranges.size(); ++r)
            anyGap = anyGap || gapBefore(clip, r) > kTimeEpsilon;
    }
    if (anyGap && !closeGaps && plan.problem.empty()) {
        const StreamSignature* audio = replaced ? nullptr : plan.reference.audio();
        if (!cannotReencode.empty())
            plan.closedGaps = "Gli spazi vuoti si chiudono (i pezzi uno dopo l'altro) perché il nero non si può creare: " + cannotReencode;
        else if (audio != nullptr && audio->codec != "aac")
            plan.closedGaps = "Gli spazi vuoti si chiudono (i pezzi uno dopo l'altro): il silenzio per ora si crea solo in AAC e "
                              "l'audio del formato del progetto è " + audio->codec + ".";
        else
            plan.blackGaps = true;
        plan.parametersInStream = plan.parametersInStream || plan.blackGaps;
    }
    if (plan.parametersInStream && plan.problem.empty()) {
        plan.clipParameters.resize(clips.size());
        for (std::size_t c = 0; c < clips.size(); ++c) {
            if (plan.reencode[c])
                continue;
            const StreamSignature* v = findStream(plan.signatures[c].streams, StreamKind::Video);
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

// Codec tag dell'originale se il contenitore di uscita lo accetta, altrimenti 0 (lo sceglie il muxer).
inline unsigned codecTagFor(const AVOutputFormat* format, const AVCodecParameters* par)
{
    if (format->codec_tag == nullptr)
        return par->codec_tag;
    if (par->codec_tag != 0 && av_codec_get_id(format->codec_tag, par->codec_tag) == par->codec_id)
        return par->codec_tag;
    return 0;
}

// Taglia un pacchetto PCM alla parte dentro `range` (al campione). false se non ne resta niente.
inline bool trimPcm(AVPacket* pkt, const StreamCopy& copy, double time, double tb, const TimeRange& range)
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

// Per un pacchetto audio copiato: false se non va scritto in `range`; il PCM viene tagliato al campione. `time` è il
// tempo del pacchetto nel file originale (senza startOffset). `continues`: il tratto continua nella parte dopo, quindi
// il pacchetto a cavallo della fine resta qui.
inline bool keepAudioInRange(AVPacket* pkt, const StreamCopy& copy, double time, double tb, const TimeRange& range, bool continues)
{
    if (copy.pcmBlockAlign > 0)
        return trimPcm(pkt, copy, time, tb, range);
    const double end = time + std::max<std::int64_t>(pkt->duration, 0) * tb;
    return !(time < range.start - kTimeEpsilon || (continues ? time >= range.end - kTimeEpsilon : end > range.end + kTimeEpsilon));
}

// Porta il demuxer al fotogramma chiave del flusso `index` in `seconds` (tempo mostrato) o a quello subito prima; se non
// si può, all'inizio del file.
inline void seekBefore(AVFormatContext* fmt, int index, double seconds, double startOffset)
{
    const auto ts = std::llround((seconds + startOffset) / av_q2d(fmt->streams[index]->time_base));
    if (av_seek_frame(fmt, index, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(fmt, index, 0, AVSEEK_FLAG_BACKWARD);
}

// Copia i dati extra nei parametri del codec (con il riempimento che chiede FFmpeg).
inline void setExtradata(AVCodecParameters* par, const std::uint8_t* data, std::size_t size)
{
    av_freep(&par->extradata);
    par->extradata_size = 0;
    par->extradata = static_cast<std::uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (par->extradata == nullptr)
        throw MediaError(kOutOfMemoryForOutput);
    std::memcpy(par->extradata, data, size);
    par->extradata_size = static_cast<int>(size);
}

// Il codec tag per un video con i parametri nel flusso (avc3 / hev1 invece di avc1 / hvc1), se il contenitore lo
// ammette; altrimenti 0.
inline unsigned inStreamTag(const AVOutputFormat* format, AVCodecID codec)
{
    if (format->codec_tag == nullptr)
        return 0;
    const unsigned tag = codec == AV_CODEC_ID_HEVC ? MKTAG('h', 'e', 'v', '1') : MKTAG('a', 'v', 'c', '3');
    return av_codec_get_id(format->codec_tag, tag) == codec ? tag : 0;
}

inline void requireCodec(const AVOutputFormat* format, AVCodecID codec)
{
    if (avformat_query_codec(format, codec, FF_COMPLIANCE_NORMAL) == 0)
        throw MediaError("Il formato " + std::string(format->name) + " non può contenere il codec " + avcodec_get_name(codec) + ".");
}

// Fotogrammi al secondo del formato del progetto, come frazione (30 se non li dice).
inline AVRational projectFrameRate(const StreamSignature& format)
{
    return av_d2q(format.frameRate > 0 ? format.frameRate : 30.0, 1001000);
}

// Encoder video nel formato del progetto (H.264 o HEVC) con i suoi parametri di codifica già letti, per i tratti da
// ricodificare e per il nero.
struct ProjectVideoEncoder
{
    ProjectVideoEncoder(const StreamSignature& format, AVRational fps, Rational timeBase, QualityPreset quality)
        : encoder(settingsFor(format, fps, timeBase, quality))
    {
        AVCodecParameters* par = avcodec_parameters_alloc();
        if (par == nullptr)
            throw MediaError(kOutOfMemory);
        encoder.copyParameters(par);
        const auto sets = parameterNalsOf(par->codec_id, std::vector<std::uint8_t>(par->extradata, par->extradata + par->extradata_size));
        avcodec_parameters_free(&par);
        if (!sets)
            throw MediaError("L'encoder " + encoder.info().ffmpegName + " non ha dato i parametri di codifica.");
        parameters = *sets;
    }

    VideoEncoder encoder;
    ParameterSets parameters;

private:
    static VideoEncoderSettings settingsFor(const StreamSignature& format, AVRational fps, Rational timeBase, QualityPreset quality)
    {
        VideoEncoderSettings settings;
        settings.codec = format.codec == "hevc" ? VideoCodec::Hevc : VideoCodec::H264;
        settings.width = format.width;
        settings.height = format.height;
        settings.frameRate = {fps.num, fps.den};
        settings.timeBase = timeBase;
        settings.quality = quality;
        return settings;
    }
};

// Fotogramma nero (gamma TV: luminanza 16, colore 128) della misura data.
inline FramePtr makeBlackFrame(int width, int height)
{
    FramePtr black(av_frame_alloc());
    if (!black)
        throw MediaError(kOutOfMemory);
    black->format = AV_PIX_FMT_YUV420P;
    black->width = width;
    black->height = height;
    if (av_frame_get_buffer(black.get(), 0) < 0)
        throw MediaError(kOutOfMemory);
    const std::ptrdiff_t linesizes[4] = {black->linesize[0], black->linesize[1], black->linesize[2], black->linesize[3]};
    av_image_fill_black(black->data, linesizes, AV_PIX_FMT_YUV420P, AVCOL_RANGE_MPEG, width, height);
    return black;
}

// Codifica `samples` campioni di silenzio (a blocchi di 1024), con un solo fotogramma riusato per tutti.
inline void encodeSilence(AudioEncoder& encoder, int sampleRate, int channels, std::int64_t samples, const PacketSink& sink)
{
    constexpr int kBlock = 1024;
    FramePtr silence(av_frame_alloc());
    if (!silence)
        throw MediaError(kOutOfMemory);
    silence->format = AV_SAMPLE_FMT_FLTP;
    silence->sample_rate = sampleRate;
    silence->nb_samples = kBlock;
    av_channel_layout_default(&silence->ch_layout, std::max(1, channels));
    if (av_frame_get_buffer(silence.get(), 0) < 0)
        throw MediaError(kOutOfMemory);
    av_samples_set_silence(silence->extended_data, 0, kBlock, silence->ch_layout.nb_channels, AV_SAMPLE_FMT_FLTP);
    for (std::int64_t remaining = samples; remaining > 0; remaining -= silence->nb_samples) {
        silence->nb_samples = static_cast<int>(std::min<std::int64_t>(remaining, kBlock));
        encoder.encode(silence.get(), sink);
    }
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
inline void describeAudio(StreamCopy& copy, const AVCodecParameters* par)
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
inline InputFile openClip(const std::string& path, bool withAudio)
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
            const double fps = guessFrameRate(file.ctx.get(), in);
            copy.frameDuration = fps > 0 ? 1.0 / fps : 0.0;
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
inline std::optional<std::vector<std::uint8_t>> encodeSilentFrame(const AVCodecParameters* par)
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
        if (defaultOrCopyLayout(&ctx->ch_layout, par->ch_layout) < 0)
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
inline std::vector<std::uint8_t> withAdtsHeader(const std::vector<std::uint8_t>& raw, const AVCodecParameters* par)
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
inline bool isSilenceableMpegFrame(const AVPacket* pkt, const AVCodecParameters* par)
{
    return (par->codec_id == AV_CODEC_ID_MP3 || par->codec_id == AV_CODEC_ID_MP2) && pkt->size > 36 && pkt->data[0] == 0xFF
        && (pkt->data[1] & 0xE0) == 0xE0;
}

// Fotogramma MP3 / MP2 di silenzio, della stessa misura: si tiene l'intestazione (stessa frequenza, canali e
// bitrate), si toglie il controllo CRC e si azzera tutto il resto. Per MP3 vuol dire nessun dato per nessuna banda
// (lunghezze a zero nelle informazioni laterali), per MP2 nessun bit assegnato: in entrambi i casi silenzio. Non serve
// un encoder (quello MP3 di solito non c'è).
inline void silenceMpegFrame(AVPacket* pkt)
{
    if (av_packet_make_writable(pkt) < 0)
        throw MediaError(kOutOfMemory);
    pkt->data[1] |= 0x01;  // protection_bit = 1: niente CRC
    std::memset(pkt->data + 4, 0, static_cast<std::size_t>(pkt->size) - 4);
}

inline bool hasAdtsHeader(const AVPacket* pkt)
{
    return pkt->size >= 7 && pkt->data[0] == 0xFF && (pkt->data[1] & 0xF6) == 0xF0;
}

// Audio in un tratto silenziato ([seqStart, seqEnd) = dove cade il pacchetto nella sequenza). Il PCM viene azzerato
// al campione; un pacchetto compresso il cui centro cade nel tratto prende il contenuto di un blocco di silenzio
// codificato con gli stessi parametri (stessi tempi e durata, così nel file creato l'audio resta continuo), oppure,
// con MuteMethod::Omit o se il silenzio non si sa codificare, viene tolto. false = pacchetto da non scrivere.
inline bool applyMute(AVPacket* pkt, StreamCopy& copy, const AVCodecParameters* par, double seqStart, double seqEnd,
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
inline bool canMux(const AVOutputFormat* format, const std::string& extension, AVFormatContext* input, int streamIndex)
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
inline std::string replacementProblem(AVFormatContext* ctx, int index, const std::string& audioPath,
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


// Una esportazione: il costruttore controlla gli argomenti, apre i file e prepara i flussi del file creato; run() scrive
// i tratti uno dopo l'altro (copiati, ricodificati o spazi vuoti) e chiude il file.
class ExportJob
{
public:
    // `analysis`: l'analisi del piano, se è già stata fatta con gli stessi argomenti (si evita di aprire di nuovo i file).
    ExportJob(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
              const ExportProgress& progress, MuteMethod muteMethod, const ProjectFormat* format,
              const ReencodeOptions& reencode, const PlanAnalysis* analysis = nullptr);
    ExportResult run();

private:
    // Preparazione (nel costruttore).
    void addClipStreams();         // i flussi del primo file, con gli stessi parametri dei codec
    void addProjectStreams();      // i flussi del formato del progetto (se il primo file si ricodifica)
    void addReplacementStream();   // un flusso audio in più, copiato dal file audio sostitutivo
    void findOutputStreams();      // video e audio dei file nel file creato, e la lunghezza dei NAL

    // Scrittura.
    void removePartial();
    void copyRange(InputFile& current, std::size_t c, const Piece& piece);
    void keepDtsIncreasing(AVPacket* pkt, int outputIndex);
    void extend(double start, double end);
    void writePacket(AVPacket* pkt, int index);
    void pumpReplacement(double until, double end);
    void writeCopied(AVPacket* pkt, StreamCopy& copy, const AVStream* in, double shiftSeconds, std::size_t c,
                               double& videoEnd, double& audioEnd);
    bool copyAudioPacket(AVPacket* pkt, StreamCopy& copy, const AVStream* in, double time, double start,
                                   const TimeRange& range, bool continues, double shiftSeconds, std::size_t c, double& audioEnd);
    PacketSink videoSinkFor(ProjectVideoEncoder& video, std::int64_t defaultDuration, double frameSeconds);
    PacketSink audioSinkFor(int rate, StreamCopy* muting, double& audioEnd);
    std::pair<double, double> reencodeRange(InputFile& current, const TimeRange& range, std::size_t c, bool continues);
    std::pair<double, double> writeBlack(double seconds);
    // Le scritte che si vedono al tempo `sequenceTime` del montaggio, già disegnate alla misura del fotogramma del progetto
    // (dal livello più basso al più alto); vuoto se non ce ne sono.
    std::vector<const TextImage*> textsAt(double sequenceTime);
    // Il fotogramma con le scritte sopra (alla misura del progetto), da dare all'encoder al posto di `frame`.
    const AVFrame* withText(const AVFrame* frame, const std::vector<const TextImage*>& texts);

    const std::vector<ClipRanges>& clips_;
    const std::string& outputPath_;
    const AudioEdit& audio_;
    const ExportProgress& progress_;
    const MuteMethod muteMethod_;
    const ReencodeOptions& reencode_;

    double totalLength_ = 0.0;  // del montaggio, spazi vuoti compresi
    int rangeCount_ = 0;
    const bool replaced_;  // c'è una traccia sostitutiva
    ReplacementSource replacement_;
    PlanAnalysis plan_;
    InputFile first_;
    int clipStreams_ = 0;
    OutputPtr output_;
    int streamCount_ = 0;
    int outVideo_ = -1;  // video e audio dei file (non la traccia sostitutiva) nel file creato, per i tratti ricodificati
    int outAudio_ = -1;
    int outLength_ = 4;  // byte della lunghezza davanti ai NAL nei pacchetti video del file creato (per i parametri nel flusso)

    // Stato della scrittura.
    std::map<int, TextImage> textImages_;  // per id di scritta: disegnata una volta sola
    std::unique_ptr<TextFrameBlender> blender_;
    PacketPtr packet_;
    ExportResult result_;
    std::vector<std::int64_t> lastDts_;  // per ogni flusso del file creato: ultimo tempo di decodifica scritto
    double doneLength_ = 0.0;
    double outputPosition_ = 0.0;  // dove inizia, nel file creato, il tratto in corso
    double firstTime_ = std::numeric_limits<double>::max();
    double lastEnd_ = 0.0;
};

}  // namespace segment
}  // namespace editor::core
