#include "EditorCore/SegmentExportInternal.h"

// Le funzioni dell'esportazione che non sono né ExportJob né SegmentExport.cpp: analisi del piano e suddivisione
// dei tratti, parametri e NAL H.264/HEVC, silenzio AAC/MPEG, apertura dei file. Prima erano `inline` nell'intestazione
// (e quindi compilate due volte).

namespace editor::core::segment {

namespace {

// Legge i bit di un NAL senza i byte di protezione (00 00 03), per i campi dell'SPS HEVC.
struct NalBits
{
    explicit NalBits(const std::vector<std::uint8_t>& nal)
    {
        for (std::size_t i = 0; i < nal.size(); ++i) {
            if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0)
                continue;
            bytes.push_back(nal[i]);
        }
    }
    unsigned bit()
    {
        const std::size_t byte = position >> 3;
        const unsigned value = byte < bytes.size() ? (bytes[byte] >> (7 - (position & 7))) & 1u : 0u;
        ++position;
        return value;
    }
    unsigned bits(int count)
    {
        unsigned value = 0;
        for (int i = 0; i < count; ++i)
            value = value << 1 | bit();
        return value;
    }
    unsigned ue()
    {
        int zeros = 0;
        while (zeros < 31 && bit() == 0)
            ++zeros;
        return zeros == 0 ? 0u : (1u << zeros) - 1u + bits(zeros);
    }
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
};

}  // namespace

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

bool rewriteNals(AVPacket* pkt, int fromSize, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix)
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

std::optional<ParameterSets> parameterNalsOf(AVCodecID codec, const std::vector<std::uint8_t>& extra)
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

std::vector<std::uint8_t> buildAvcC(const ParameterSets& sets)
{
    std::vector<const std::vector<std::uint8_t>*> sps;
    std::vector<const std::vector<std::uint8_t>*> pps;
    for (const auto& unit : sets.nals) {
        if (unit.size() < 4)
            continue;
        if ((unit[0] & 0x1f) == 7)
            sps.push_back(&unit);
        else if ((unit[0] & 0x1f) == 8)
            pps.push_back(&unit);
    }
    if (sps.empty() || pps.empty())
        return {};
    std::vector<std::uint8_t> out{1, (*sps[0])[1], (*sps[0])[2], (*sps[0])[3], 0xFF, static_cast<std::uint8_t>(0xE0 | sps.size())};
    const auto add = [&out](const std::vector<std::uint8_t>& unit) {
        out.push_back(static_cast<std::uint8_t>(unit.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(unit.size() & 0xff));
        out.insert(out.end(), unit.begin(), unit.end());
    };
    for (const auto* unit : sps)
        add(*unit);
    out.push_back(static_cast<std::uint8_t>(pps.size()));
    for (const auto* unit : pps)
        add(*unit);
    return out;
}

std::vector<std::uint8_t> buildHvcC(const ParameterSets& sets)
{
    const std::vector<std::uint8_t>* vps = nullptr;
    const std::vector<std::uint8_t>* sps = nullptr;
    const std::vector<std::uint8_t>* pps = nullptr;
    for (const auto& unit : sets.nals) {
        if (unit.size() < 3)
            continue;
        const int type = (unit[0] >> 1) & 0x3f;
        if (type == 32 && vps == nullptr)
            vps = &unit;
        else if (type == 33 && sps == nullptr)
            sps = &unit;
        else if (type == 34 && pps == nullptr)
            pps = &unit;
    }
    if (vps == nullptr || sps == nullptr || pps == nullptr)
        return {};
    // SPS: 2 byte d'intestazione del NAL, poi VPS id (4 bit), sottolivelli (3), nidificazione (1) e profile_tier_level.
    NalBits sp(*sps);
    sp.bits(16);
    sp.bits(4);
    const unsigned subLayers = sp.bits(3) + 1;
    sp.bits(1);
    const unsigned profileByte = sp.bits(8);
    const unsigned compatibility = sp.bits(32);
    const unsigned constraintsHigh = sp.bits(16);  // (in due istruzioni: l'ordine di lettura conta)
    const unsigned constraintsLow = sp.bits(32);
    const std::uint64_t constraints = static_cast<std::uint64_t>(constraintsHigh) << 32 | constraintsLow;
    const unsigned level = sp.bits(8);
    unsigned chroma = 1, lumaDepth = 8, chromaDepth = 8;
    if (subLayers == 1) {  // con più sottolivelli il profilo continua con altri campi: si lasciano i valori comuni
        sp.ue();  // id dell'SPS
        chroma = sp.ue();
        if (chroma == 3)
            sp.bit();
        sp.ue();  // larghezza
        sp.ue();  // altezza
        if (sp.bit() != 0) {  // finestra di ritaglio
            for (int i = 0; i < 4; ++i)
                sp.ue();
        }
        lumaDepth = sp.ue() + 8;
        chromaDepth = sp.ue() + 8;
    }
    std::vector<std::uint8_t> out{1, static_cast<std::uint8_t>(profileByte)};
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(compatibility >> shift));
    for (int shift = 40; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(constraints >> shift));
    out.push_back(static_cast<std::uint8_t>(level));
    out.push_back(0xF0);  // segmentazione spaziale minima: non detta
    out.push_back(0x00);
    out.push_back(0xFC);  // parallelismo: non detto
    out.push_back(static_cast<std::uint8_t>(0xFC | (chroma & 3)));
    out.push_back(static_cast<std::uint8_t>(0xF8 | ((lumaDepth - 8) & 7)));
    out.push_back(static_cast<std::uint8_t>(0xF8 | ((chromaDepth - 8) & 7)));
    out.push_back(0);  // fotogrammi al secondo medi: non detti
    out.push_back(0);
    out.push_back(static_cast<std::uint8_t>((std::min(subLayers, 7u) << 3) | 3));  // sottolivelli, lunghezze su 4 byte
    out.push_back(3);  // tre gruppi: VPS, SPS, PPS
    for (const std::vector<std::uint8_t>* unit : {vps, sps, pps}) {
        out.push_back(static_cast<std::uint8_t>(0x80 | ((*unit)[0] >> 1 & 0x3f)));  // completo + tipo del NAL
        out.push_back(0);
        out.push_back(1);
        out.push_back(static_cast<std::uint8_t>(unit->size() >> 8));
        out.push_back(static_cast<std::uint8_t>(unit->size() & 0xff));
        out.insert(out.end(), unit->begin(), unit->end());
    }
    return out;
}

bool encodedToLengthPrefixed(AVPacket* pkt, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix)
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

std::vector<Piece> splitForPreciseCut(const TimeRange& range, const std::vector<double>& keyframes, bool bFrames)
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

std::vector<TimeRange> textCoverage(const TextTracks* texts)
{
    std::vector<TimeRange> spans;
    if (texts == nullptr)
        return spans;
    for (const TextClip& clip : texts->baseLayer().clips) {  // le scritte su V1
        if (!clip.text.empty() && clip.length > kTimeEpsilon)
            spans.push_back({clip.position, clip.position + clip.length});
    }
    for (const TextLayer& layer : texts->layers()) {
        for (const TextClip& clip : layer.clips) {
            if (!clip.text.empty() && clip.length > kTimeEpsilon)
                spans.push_back({clip.position, clip.position + clip.length});
        }
        for (const LayerPiece& piece : layer.pieces) {  // i pezzi di video dei livelli, composti sopra V1
            if (piece.length() > kTimeEpsilon)
                spans.push_back({piece.position, piece.endPosition()});
        }
    }
    return mergeRanges(std::move(spans));
}

std::vector<TransitionPart> splitByTransition(const TimeRange& range, int index, const std::vector<ExportTransition>* transitions)
{
    int starting = -1;  // la transizione che comincia in questo tratto (sta sulla giunzione di prima)
    int ending = -1;    // quella che finisce in questo tratto
    if (transitions != nullptr) {
        for (std::size_t i = 0; i < transitions->size(); ++i) {
            if ((*transitions)[i].beforeRange == index)
                ending = static_cast<int>(i);
            else if ((*transitions)[i].beforeRange == index - 1)
                starting = static_cast<int>(i);
        }
    }
    std::vector<TransitionPart> parts;
    double at = range.start;
    if (starting >= 0) {
        const double to = std::min(range.end, range.start + (*transitions)[static_cast<std::size_t>(starting)].after);
        if (to - at > kTimeEpsilon) {
            parts.push_back({{at, to}, starting, false});
            at = to;
        }
    }
    double middleEnd = range.end;
    if (ending >= 0)
        middleEnd = std::max(at, range.end - (*transitions)[static_cast<std::size_t>(ending)].before);
    if (middleEnd - at > kTimeEpsilon) {
        parts.push_back({{at, middleEnd}, -1, false});
        at = middleEnd;
    }
    if (ending >= 0 && range.end - at > kTimeEpsilon)
        parts.push_back({{at, range.end}, ending, true});
    if (parts.empty())
        return {{range, -1, false}};
    parts.front().range.start = range.start;
    parts.back().range.end = range.end;
    return parts;
}

std::vector<std::pair<TimeRange, bool>> splitByText(const TimeRange& range, double sequenceStart,
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

PlanAnalysis analyzePlan(const std::vector<ClipRanges>& clips, const ProjectFormat* format, bool replaced, bool reencodeAll,
                         bool closeGaps, const TextTracks* texts,
                         const std::vector<ExportTransition>* transitions, std::optional<EncoderBackend> backend,
                         const VideoChange* change, double tailSeconds)
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
    // Il video voluto dall'utente: il formato di riferimento cambia, quindi ogni pezzo si ricodifica. I parametri di
    // codifica non ci sono ancora (li dà l'encoder quando parte l'esportazione).
    if (change != nullptr) {
        for (StreamSignature& s : plan.reference.streams) {
            if (s.kind != StreamKind::Video)
                continue;
            const std::string codec = change->codec ? videoCodecId(*change->codec) : s.codec;
            const int width = change->width > 0 && change->height > 0 ? change->width : s.width;
            const int height = change->width > 0 && change->height > 0 ? change->height : s.height;
            const double fps = change->frameRate > 0 ? change->frameRate : s.frameRate;
            const bool sameRate = s.frameRate > 0 && std::abs(fps - s.frameRate) <= s.frameRate * 0.001;
            if (codec != s.codec || width != s.width || height != s.height || !sameRate) {
                s.codec = codec;
                s.width = width;
                s.height = height;
                s.frameRate = fps;
                s.profile.clear();
                s.extradata.clear();
                plan.videoChanged = true;
            }
            break;
        }
        if (plan.videoChanged)
            plan.reencode.assign(clips.size(), true);
    }
    // Un formato AV1 non ripete i parametri nel flusso: pezzi copiati e pezzi ricodificati non si possono mescolare. Con
    // "Ricodifica tutto" si ricodifica tutto, come per un cambio di video (parametri dati dall'encoder).
    if (reencodeAll && !plan.videoChanged) {
        for (StreamSignature& s : plan.reference.streams) {
            if (s.kind == StreamKind::Video && s.codec == videoCodecId(VideoCodec::Av1)) {
                s.profile.clear();
                s.extradata.clear();
                plan.videoChanged = true;
            }
            if (s.kind == StreamKind::Video)
                break;
        }
    }
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
    } else if (video->codec != "h264" && video->codec != "hevc" && !(plan.videoChanged && video->codec == "av1")) {
        cannotReencode = "Il formato del progetto (" + plan.reference.sourceName + ") è " + video->codec
                       + ": la ricodifica per ora si fa solo in H.264 e HEVC (con AV1 solo con «Ricodifica tutto»).";
    } else {
        const VideoCodec codec = videoCodecFromName(video->codec).value_or(VideoCodec::H264);
        if (!findVideoEncoder(codec, backend)) {
            cannotReencode = backend ? "L'encoder scelto (" + backendName(*backend) + ") non è disponibile per " + codecName(codec)
                                           + " su questo PC: sceglierne un altro (vedi Aiuto > Informazioni)."
                                     : "Per ricodificare in " + codecName(codec) + " serve un encoder, ma su questo PC non ce n'è "
                                       "uno funzionante (vedi Aiuto > Informazioni).";
        } else if (!plan.videoChanged && !parameterSetsOf(codecIdOf(video->codec), video->extradata)) {
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
    bool transitionNeeded = false;
    bool transformNeeded = false;
    int rangeIndex = -1;  // posizione del tratto nel piano (tutti i file, in ordine)
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
            ++rangeIndex;
            sequence += gapBefore(clips[c], r);
            const double sequenceStart = sequence;
            sequence += range.length();
            const Transform transform = transformOf(clips[c], r);
            std::vector<Piece> all;
            for (const TransitionPart& part : splitByTransition(range, rangeIndex, transitions)) {
                if (part.transition >= 0) {
                    // Dentro una transizione: si ricodifica con i due pezzi fusi.
                    Piece piece{part.range, true};
                    piece.transition = part.transition;
                    piece.transitionEnding = part.ending;
                    all.push_back(piece);
                    transitionNeeded = true;
                    preciseNeeded = preciseNeeded || cannotReencode.empty();
                    continue;
                }
                const TimeRange partRange = part.range;
                const double partStart = sequenceStart + (partRange.start - range.start);
                if (!transform.isIdentity()) {
                    // Un tratto trasformato si ricodifica tutto: la trasformazione si applica al fotogramma decodificato.
                    Piece piece{partRange, true};
                    piece.transformed = true;
                    piece.transform = transform;
                    all.push_back(piece);
                    transformNeeded = true;
                    preciseNeeded = preciseNeeded || cannotReencode.empty();
                    continue;
                }
                if (plan.reencode[c] || own == nullptr || keyframes.empty()) {
                    all.push_back({partRange, plan.reencode[c]});
                    continue;
                }
                // Le parti sotto una scritta si ricodificano (la scritta si disegna sul fotogramma); le altre come sempre.
                std::vector<Piece> pieces;
                bool anyText = false;
                for (const auto& [sub, covered] : splitByText(partRange, partStart, coverage)) {
                    if (covered) {
                        anyText = true;
                        Piece piece{sub, true};
                        piece.text = true;
                        pieces.push_back(piece);
                        continue;
                    }
                    std::vector<Piece> own_pieces = splitForPreciseCut(sub, keyframes, own->bFrames);
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
                    pieces = {{{snapToKeyframe(keyframes, partRange.start), partRange.end}, false}};
                    if (precise && !anyText && plan.impreciseCuts.empty())
                        plan.impreciseCuts = "Il taglio preciso non è possibile, quindi IN va al fotogramma chiave più vicino prima: "
                                           + cannotReencode;
                }
                preciseNeeded = preciseNeeded || ((precise || anyText) && cannotReencode.empty());
                all.insert(all.end(), pieces.begin(), pieces.end());
            }
            // Le parti dello stesso tratto: l'audio continua dall'una all'altra.
            for (std::size_t i = 0; i + 1 < all.size(); ++i)
                all[i].continues = true;
            plan.pieces[c].push_back(std::move(all));
        }
    }

    if (anyReencoded || preciseNeeded) {
        if (anyReencoded) {
            plan.problem = cannotReencode;
            if (!plan.problem.empty() && video != nullptr && video->codec != "h264" && video->codec != "hevc")
                plan.problem += " Scegli come formato un file H.264 o HEVC (Montaggio > Impostazioni del progetto), oppure togli i "
                                "pezzi segnati con R, oppure spunta «Ricodifica tutto» nella finestra Esporta.";
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
    if (transformNeeded && !cannotReencode.empty() && plan.problem.empty())
        plan.problem = "Scala, posizione, rotazione e ritaglio si applicano ricodificando i tratti trasformati, ma non si può: "
                     + cannotReencode;
    if (transitionNeeded && !cannotReencode.empty() && plan.problem.empty())
        plan.problem = "Le transizioni si fondono ricodificando i tratti che toccano, ma non si può: " + cannotReencode;
    // Spazi vuoti: nero e silenzio nel formato del progetto (con l'encoder, come la ricodifica), se si può.
    bool anyGap = false;
    for (const ClipRanges& clip : clips) {
        for (std::size_t r = 0; r < clip.ranges.size(); ++r)
            anyGap = anyGap || gapBefore(clip, r) > kTimeEpsilon;
    }
    const bool tail = tailSeconds > kTimeEpsilon;
    const bool gapsWanted = anyGap && !closeGaps;
    if ((gapsWanted || tail) && plan.problem.empty()) {
        const StreamSignature* audio = replaced ? nullptr : plan.reference.audio();
        // Il nero (e il silenzio) si può creare? Se no, gli spazi si chiudono e la coda non si esporta.
        std::string blackProblem;
        if (!cannotReencode.empty())
            blackProblem = " perché il nero non si può creare: " + cannotReencode;
        else if (audio != nullptr && audio->codec != "aac")
            blackProblem = ": il silenzio per ora si crea solo in AAC e "
                           "l'audio del formato del progetto è " + audio->codec + ".";
        if (blackProblem.empty()) {
            plan.blackGaps = gapsWanted;
            plan.blackTail = tail;
        } else if (gapsWanted) {
            plan.closedGaps = "Gli spazi vuoti si chiudono (i pezzi uno dopo l'altro) " + blackProblem;
        }
        plan.parametersInStream = plan.parametersInStream || plan.blackGaps || plan.blackTail;
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

void setExtradata(AVCodecParameters* par, const std::uint8_t* data, std::size_t size)
{
    av_freep(&par->extradata);
    par->extradata_size = 0;
    par->extradata = static_cast<std::uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (par->extradata == nullptr)
        throw MediaError(kOutOfMemoryForOutput);
    std::memcpy(par->extradata, data, size);
    par->extradata_size = static_cast<int>(size);
}

FramePtr makeBlackFrame(int width, int height, AVPixelFormat pixelFormat)
{
    FramePtr black(av_frame_alloc());
    if (!black)
        throw MediaError(kOutOfMemory);
    black->format = pixelFormat;
    black->width = width;
    black->height = height;
    if (av_frame_get_buffer(black.get(), 0) < 0)
        throw MediaError(kOutOfMemory);
    const std::ptrdiff_t linesizes[4] = {black->linesize[0], black->linesize[1], black->linesize[2], black->linesize[3]};
    av_image_fill_black(black->data, linesizes, pixelFormat, AVCOL_RANGE_MPEG, width, height);
    return black;
}

void encodeSilence(AudioEncoder& encoder, int sampleRate, int channels, std::int64_t samples, const PacketSink& sink)
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
            const double fps = guessFrameRate(file.ctx.get(), in);
            copy.frameDuration = fps > 0 ? 1.0 / fps : 0.0;
        } else {
            describeAudio(copy, par);
        }
    }
    return file;
}

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

}  // namespace editor::core::segment
