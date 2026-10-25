#include "EditorCore/SegmentExportInternal.h"

namespace editor::core {

using namespace detail;
using namespace segment;

namespace segment {

ExportJob::ExportJob(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                     const ExportProgress& progress, MuteMethod muteMethod, const ProjectFormat* format,
                     const ReencodeOptions& reencode, const PlanAnalysis* analysis)
    : clips_(clips)
    , outputPath_(outputPath)
    , audio_(audio)
    , progress_(progress)
    , muteMethod_(muteMethod)
    , reencode_(reencode)
    , replaced_(audio.replacement.has_value())
{
    for (const ClipRanges& clip : clips_) {
        for (std::size_t r = 0; r < clip.ranges.size(); ++r) {
            const TimeRange& range = clip.ranges[r];
            if (!(range.end > range.start + kTimeEpsilon))
                throw MediaError("OUT deve stare dopo IN.");
            totalLength_ += range.length() + gapBefore(clip, r);
            ++rangeCount_;
        }
        if (samePath(clip.path, outputPath_))
            throw MediaError("Il file di destinazione deve essere diverso dagli originali.");
    }
    if (rangeCount_ == 0)
        throw MediaError("Non c'è nessun tratto da esportare.");
    // Traccia sostitutiva: il file audio si apre una volta sola, per il controllo e poi per la copia.
    if (replaced_) {
        if (samePath(audio_.replacement->path, outputPath_))
            throw MediaError("Il file di destinazione deve essere diverso dall'audio sostitutivo.");
        try {
            replacement_.ctx = openInput(audio_.replacement->path);
        } catch (const std::exception& e) {
            throw MediaError(e.what());
        }
        replacement_.stream = findMainStream(replacement_.ctx.get(), AVMEDIA_TYPE_AUDIO);
        if (const std::string problem =
                replacementProblem(replacement_.ctx.get(), replacement_.stream, audio_.replacement->path, outputPath_);
            !problem.empty())
            throw MediaError(problem);
    }

    // Che cosa succede a ogni file: si copia (con i suoi parametri nel flusso, se diversi) o si ricodifica.
    plan_ = analysis != nullptr ? *analysis : analyzePlan(clips_, format, replaced_, reencode_.all, reencode_.closeGaps);
    if (!plan_.problem.empty())
        throw ClipsNotJoinable(plan_.problem);

    first_ = openClip(clips_.front().path, !replaced_);
    clipStreams_ = first_.copied;
    AVFormatContext* rawOutput = nullptr;
    if (avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, outputPath_.c_str()) < 0 || rawOutput == nullptr)
        throw MediaError("Dall'estensione non si capisce in che formato salvare: " + outputPath_);
    output_.reset(rawOutput);


    addClipStreams();
    addProjectStreams();
    addReplacementStream();
    findOutputStreams();
}

void ExportJob::addClipStreams()
{
    // Flussi del file creato: quelli del primo file, con gli stessi parametri dei codec. Se il primo file si ricodifica,
    // quelli del formato del progetto.
    for (unsigned i = 0; !plan_.reencode.front() && i < first_.ctx->nb_streams; ++i) {
        if (first_.streams[i].output < 0)
            continue;
        const AVStream* in = first_.ctx->streams[i];
        const AVCodecParameters* par = in->codecpar;
        requireCodec(output_->oformat, par->codec_id);
        AVStream* out = avformat_new_stream(output_.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, par) < 0)
            throw MediaError(kOutOfMemoryForOutput);
        out->codecpar->codec_tag = codecTagFor(output_->oformat, par);
        // H.264 da file con livelli diversi: il file unito dichiara il livello più alto (il resto dei parametri
        // coincide, l'ha verificato classifyClip).
        if (par->codec_id == AV_CODEC_ID_H264 && !plan_.signatures.empty()) {
            const int index = first_.streams[i].output;
            int level = 0;
            for (std::size_t s = 0; s < plan_.signatures.size(); ++s) {
                if (!plan_.reencode[s] && index < static_cast<int>(plan_.signatures[s].streams.size()))
                    level = std::max(level, maxH264Level(parseH264Config(plan_.signatures[s].streams[index].extradata)));
            }
            const std::vector<std::uint8_t> current(par->extradata, par->extradata + par->extradata_size);
            if (level > maxH264Level(parseH264Config(current))) {
                const std::vector<std::uint8_t> raised = withH264Level(current, level);
                setExtradata(out->codecpar, raised.data(), raised.size());
                out->codecpar->level = level;
            }
        }
        // Parametri nel flusso: in un MP4 il video lo dichiara (avc3 / hev1 invece di avc1 / hvc1).
        if (plan_.parametersInStream && par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (const unsigned tag = inStreamTag(output_->oformat, par->codec_id))
                out->codecpar->codec_tag = tag;
        }
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);
    }
}

void ExportJob::addProjectStreams()
{
    if (plan_.reencode.front()) {
        for (const StreamSignature& s : plan_.reference.streams) {
            AVStream* out = avformat_new_stream(output_.get(), nullptr);
            if (out == nullptr)
                throw MediaError(kOutOfMemoryForOutput);
            AVCodecParameters* par = out->codecpar;
            par->codec_id = codecIdOf(s.codec);
            requireCodec(output_->oformat, par->codec_id);
            if (!s.extradata.empty())
                setExtradata(par, s.extradata.data(), s.extradata.size());
            if (s.kind == StreamKind::Video) {
                par->codec_type = AVMEDIA_TYPE_VIDEO;
                par->width = s.width;
                par->height = s.height;
                par->format = av_get_pix_fmt(s.pixelFormat.c_str());
                par->sample_aspect_ratio = {1, 1};
                par->codec_tag = inStreamTag(output_->oformat, par->codec_id);
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
}

void ExportJob::addReplacementStream()
{
    // Traccia sostitutiva: un flusso audio in più, copiato dal file audio (aperto e controllato sopra).
    if (replaced_) {
        replacement_.packet.reset(av_packet_alloc());
        if (!replacement_.packet)
            throw MediaError(kOutOfMemory);
        keepOnlyStream(replacement_.ctx.get(), replacement_.stream);
        replacement_.start = startOffsetSeconds(replacement_.ctx.get());
        replacement_.offset = audio_.replacement->offset;
        const AVStream* in = replacement_.ctx->streams[replacement_.stream];
        describeAudio(replacement_.copy, in->codecpar);
        AVStream* out = avformat_new_stream(output_.get(), nullptr);
        if (out == nullptr || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
            throw MediaError(kOutOfMemoryForOutput);
        out->codecpar->codec_tag = codecTagFor(output_->oformat, in->codecpar);
        out->time_base = in->time_base;
        out->disposition = AV_DISPOSITION_DEFAULT;
        replacement_.copy.output = out->index;
        // Si parte dal punto del file audio che finisce all'inizio del file creato.
        seekBefore(replacement_.ctx.get(), replacement_.stream, std::max(0.0, -replacement_.offset), replacement_.start);
    }
}

void ExportJob::findOutputStreams()
{
    streamCount_ = static_cast<int>(output_->nb_streams);
    if (streamCount_ == 0)
        throw MediaError("Il file non contiene video o audio da copiare.");
    // Video e audio dei file (non la traccia sostitutiva) nel file creato, per i tratti ricodificati.
    for (int i = 0; i < streamCount_; ++i) {
        const AVCodecParameters* par = output_->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && outVideo_ < 0)
            outVideo_ = i;
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO && outAudio_ < 0 && !(replaced_ && i == replacement_.copy.output))
            outAudio_ = i;
    }
    // Byte della lunghezza davanti ai NAL nei pacchetti video del file creato (per i parametri nel flusso).
    outLength_ = 4;
    if (plan_.parametersInStream && outVideo_ >= 0) {
        const AVCodecParameters* par = output_->streams[outVideo_]->codecpar;
        if (const auto sets = parameterSetsOf(par->codec_id, std::vector<std::uint8_t>(par->extradata, par->extradata + par->extradata_size)))
            outLength_ = sets->lengthSize;
    }
}

// Da quando si comincia a scrivere, se qualcosa va storto il file a metà va cancellato.
void ExportJob::removePartial()
{
    output_.reset();
    std::error_code ec;
    std::filesystem::remove(pathFromUtf8(outputPath_), ec);
}

// I tempi di decodifica devono sempre crescere, anche a cavallo di una giunzione (con i B-frame il primo
// fotogramma di un tratto può avere un tempo di decodifica appena prima della fine del tratto precedente):
// se serve si sposta avanti di un'unità.
void ExportJob::keepDtsIncreasing(AVPacket* pkt, int outputIndex)
{
    std::int64_t& last = lastDts_[outputIndex];
    if (pkt->dts == AV_NOPTS_VALUE)
        return;
    if (last != std::numeric_limits<std::int64_t>::min() && pkt->dts <= last) {
        pkt->dts = last + 1;
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
            pkt->pts = pkt->dts;
    }
    last = pkt->dts;
}

// Il file creato copre almeno da `start` a `end` (secondi).
void ExportJob::extend(double start, double end)
{
    firstTime_ = std::min(firstTime_, start);
    lastEnd_ = std::max(lastEnd_, end);
}

// Scrive il pacchetto (che da qui non è più suo) nel flusso `index` del file creato.
void ExportJob::writePacket(AVPacket* pkt, int index)
{
    pkt->stream_index = index;
    pkt->pos = -1;
    if (const int ret = av_interleaved_write_frame(output_.get(), pkt); ret < 0)
        throw MediaError("Errore durante la scrittura del file (" + ffmpegError(ret) + ").");
}

// Scrive la traccia sostitutiva fino al tempo `until` del file creato, senza superare `end` (la fine del
// file creato): si chiama man mano che si scrive il video, così video e audio restano mescolati nel file.
void ExportJob::pumpReplacement(double until, double end)
{
    if (!replaced_)
        return;
    AVFormatContext* input = replacement_.ctx.get();
    const AVStream* in = input->streams[replacement_.stream];
    const AVStream* out = output_->streams[replacement_.copy.output];
    const double tb = av_q2d(in->time_base);
    AVPacket* pkt = replacement_.packet.get();
    while (!replacement_.ended) {
        if (!replacement_.pending) {
            const int ret = av_read_frame(input, pkt);
            if (ret == AVERROR_EOF) {
                replacement_.ended = true;
                break;
            }
            if (ret < 0)
                throw MediaError("Errore durante la lettura dell'audio sostitutivo (" + ffmpegError(ret) + ").");
            if (pkt->stream_index != replacement_.stream || (pkt->pts == AV_NOPTS_VALUE && pkt->dts == AV_NOPTS_VALUE)) {
                av_packet_unref(pkt);
                continue;
            }
            replacement_.pending = true;
        }
        const std::int64_t ts = packetTimestamp(pkt);
        const double sourceTime = ts * tb - replacement_.start;
        double time = sourceTime + replacement_.offset;  // nel file creato
        if (time >= until)
            break;
        if (time >= end - kTimeEpsilon) {
            replacement_.ended = true;
            break;
        }
        // Da qui il pacchetto si scrive o si scarta: in entrambi i casi si libera prima della prossima lettura.
        replacement_.pending = false;
        const PacketUnref release{pkt};
        double duration = std::max<std::int64_t>(pkt->duration, 0) * tb;
        if (replacement_.copy.pcmBlockAlign > 0) {
            if (!trimPcm(pkt, replacement_.copy, sourceTime, tb, {-replacement_.offset, end - replacement_.offset}))
                continue;
            time = packetTimestamp(pkt) * tb - replacement_.start + replacement_.offset;
            duration = pkt->duration * tb;
        } else if (time < -kTimeEpsilon || time + duration > end + kTimeEpsilon) {
            continue;  // solo i pacchetti interamente dentro il file creato
        }
        if (!applyMute(pkt, replacement_.copy, in->codecpar, time, time + duration, audio_.muted, muteMethod_, result_))
            continue;
        shiftPacket(pkt, std::llround((replacement_.start - replacement_.offset) / tb));
        av_packet_rescale_ts(pkt, in->time_base, out->time_base);
        keepDtsIncreasing(pkt, replacement_.copy.output);
        extend(time, time + duration);
        writePacket(pkt, replacement_.copy.output);
    }
}

// Scrive un pacchetto copiato dal file originale (video o audio) nel file creato: tutti i tempi spostati di
// `shiftSeconds`, e per il video i parametri di codifica del file `c` se vanno ripetuti nel flusso. Aggiorna dove
// finiscono il video e l'audio scritti.
void ExportJob::writeCopied(AVPacket* pkt, StreamCopy& copy, const AVStream* in, double shiftSeconds, std::size_t c,
                           double& videoEnd, double& audioEnd)
{
    // Parametri nel flusso: davanti a ogni fotogramma chiave quelli di questo file (con la lunghezza dei NAL del
    // file creato, se diversa).
    if (plan_.parametersInStream && copy.video) {
        const ParameterSets& mine = plan_.clipParameters[c];
        const int target = outLength_;
        const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if ((key || mine.lengthSize != target)
            && !rewriteNals(pkt, mine.lengthSize, target, key ? mine.nals : std::vector<std::vector<std::uint8_t>>{}))
            throw MediaError("Un fotogramma di " + clips_[c].path + " non si legge: impossibile copiarlo.");
    }
    shiftPacket(pkt, std::llround(shiftSeconds / av_q2d(in->time_base)));
    const AVStream* out = output_->streams[copy.output];
    av_packet_rescale_ts(pkt, in->time_base, out->time_base);
    keepDtsIncreasing(pkt, copy.output);

    if (pkt->pts != AV_NOPTS_VALUE) {
        const double t = pkt->pts * av_q2d(out->time_base);
        double duration = pkt->duration * av_q2d(out->time_base);
        if (copy.video && duration <= 0)
            duration = copy.frameDuration;
        extend(t, t + duration);
        if (copy.video)
            videoEnd = std::max(videoEnd, t + duration);
        // Il decoder restituisce sempre un blocco intero, anche se il contenitore ne dichiara una parte (l'ultimo
        // blocco di un file AAC contiene il riempimento dell'encoder): in mezzo al file creato quella parte in
        // più non si può togliere, quindi se ne tiene conto.
        if (copy.audioFrameSeconds > 0)
            audioEnd = std::max(audioEnd, t + std::max(duration, copy.audioFrameSeconds));
        // La traccia sostitutiva va scritta insieme al video, poco più avanti.
        if (copy.video)
            pumpReplacement(std::min(t + 0.3, totalLength_ - 0.1), totalLength_);
    }
    writePacket(pkt, copy.output);
}

// Un pacchetto audio di un file copiato, nel tratto `range` (che comincia dopo `doneLength` secondi del
// montaggio): lo tiene solo la parte dentro il tratto, silenziata dove serve, poi lo scrive. false se non è
// stato scritto. `time` = tempo del pacchetto nel file originale (senza startOffset), `start` = startOffset.
bool ExportJob::copyAudioPacket(AVPacket* pkt, StreamCopy& copy, const AVStream* in, double time, double start,
                               const TimeRange& range, bool continues, double shiftSeconds, std::size_t c, double& audioEnd)
{
    const double tb = av_q2d(in->time_base);
    if (!keepAudioInRange(pkt, copy, time, tb, range, continues))
        return false;
    // Tratti silenziati: si confrontano con il tempo del pacchetto nella sequenza.
    const std::int64_t kept = packetTimestamp(pkt);
    const double seqStart = doneLength_ + (kept * tb - start - range.start);
    double length = std::max<std::int64_t>(pkt->duration, 0) * tb;
    if (length <= 0)
        length = copy.audioFrameSeconds;
    if (!applyMute(pkt, copy, in->codecpar, seqStart, seqStart + length, audio_.muted, muteMethod_, result_))
        return false;
    double videoEnd = 0.0;
    writeCopied(pkt, copy, in, shiftSeconds, c, videoEnd, audioEnd);
    return true;
}

// Dove vanno i pacchetti dell'encoder video del progetto: parametri nel flusso davanti a ogni fotogramma chiave,
// tempi nell'unità del flusso, e (poco più avanti) la traccia sostitutiva. `defaultDuration` = durata di un
// fotogramma nell'unità dell'encoder, per i pacchetti che non la dicono.
PacketSink ExportJob::videoSinkFor(ProjectVideoEncoder& video, std::int64_t defaultDuration, double frameSeconds)
{
    const Rational encoderTb = video.encoder.timeBase();
    AVStream* outV = output_->streams[outVideo_];
    return [&, &parameters = video.parameters, encoderTb, outV, defaultDuration, frameSeconds](AVPacket* p) {
        const bool key = (p->flags & AV_PKT_FLAG_KEY) != 0;
        if (!encodedToLengthPrefixed(p, outLength_, key ? parameters.nals : std::vector<std::vector<std::uint8_t>>{}))
            throw MediaError("Un pacchetto dell'encoder non si legge.");
        if (p->duration <= 0)
            p->duration = defaultDuration;
        av_packet_rescale_ts(p, {encoderTb.num, encoderTb.den}, outV->time_base);
        keepDtsIncreasing(p, outVideo_);
        const double t = p->pts * av_q2d(outV->time_base);
        extend(t, t + frameSeconds);
        pumpReplacement(std::min(t + 0.3, totalLength_ - 0.1), totalLength_);
        writePacket(p, outVideo_);
    };
}

// Dove vanno i pacchetti AAC dell'encoder audio del progetto, che comincia a `outputPosition` del file creato.
// `muting` = il flusso con cui applicare i tratti silenziati (vedi applyMute), nullptr = niente da silenziare.
// `audioEnd` = dove finisce l'audio scritto.
PacketSink ExportJob::audioSinkFor(int rate, StreamCopy* muting, double& audioEnd)
{
    AVStream* outA = output_->streams[outAudio_];
    return [&, &audioEnd = audioEnd, rate, outA, muting](AVPacket* p) {
        if (p->pts < 0)
            return;  // il ritardo iniziale dell'encoder (qualche millesimo): non si sente
        const double time = outputPosition_ + static_cast<double>(p->pts) / rate;
        // L'ultimo blocco può dichiarare meno campioni, ma il decoder ne restituisce comunque uno intero.
        const double duration = std::max<std::int64_t>({p->duration, 0, outA->codecpar->frame_size}) / static_cast<double>(rate);
        if (muting != nullptr) {
            const double seqStart = doneLength_ + (time - outputPosition_);
            if (!applyMute(p, *muting, outA->codecpar, seqStart, seqStart + duration, audio_.muted, muteMethod_, result_))
                return;
        }
        const std::int64_t shift = std::llround(outputPosition_ * rate);
        p->pts += shift;
        if (p->dts != AV_NOPTS_VALUE)
            p->dts += shift;
        av_packet_rescale_ts(p, {1, rate}, outA->time_base);
        keepDtsIncreasing(p, outAudio_);
        extend(time, time + duration);
        audioEnd = std::max(audioEnd, time + duration);
        writePacket(p, outAudio_);
    };
}

// Ricodifica di un tratto. Il video si decodifica e si ricodifica nel formato del progetto: a fotogrammi al
// secondo costanti, per ogni fotogramma del file creato quello dell'originale che si vede in quel momento (ripetuto
// o saltato se le frequenze sono diverse, senza deriva anche con 59,94 o una frequenza variabile); misura e
// formato dei pixel li adatta l'encoder (bande nere se le proporzioni sono diverse). L'audio si copia se è uguale
// a quello del formato, altrimenti si ricodifica in AAC (silenzio se il file non ha audio). Restituisce dove
// finiscono, nel file creato, il video e l'audio scritti.
std::pair<double, double> ExportJob::reencodeRange(InputFile& current, const TimeRange& range, std::size_t c, bool continues)
{
    AVFormatContext* input = current.ctx.get();
    const double start = current.start;
    const StreamSignature& format = *plan_.reference.video();
    const AVRational fps = projectFrameRate(format);
    const double frameSeconds = av_q2d(av_inv_q(fps));
    const int frames = std::max(1, static_cast<int>(std::llround(range.length() / frameSeconds)));
    const std::int64_t base = std::llround(outputPosition_ / frameSeconds);

    // Un encoder per tratto: alla fine si svuota, così i suoi pacchetti non si mescolano con quelli dopo.
    ProjectVideoEncoder video(format, fps, Rational{0, 1}, reencode_.quality);
    const PacketSink videoSink = videoSinkFor(video, 1, frameSeconds);

    // Decoder del video (e dell'audio, se si ricodifica).
    const auto openDecoder = [&](int index) {
        const AVStream* st = input->streams[index];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (codec == nullptr)
            throw ClipsNotJoinable("Il file " + clips_[c].path + " è in " + avcodec_get_name(st->codecpar->codec_id)
                                   + ": questa versione del programma non sa decodificarlo per ricodificarlo.");
        CodecContextPtr ctx(avcodec_alloc_context3(codec));
        avcodec_parameters_to_context(ctx.get(), st->codecpar);
        ctx->pkt_timebase = st->time_base;
        ctx->thread_count = 0;
        if (const int ret = avcodec_open2(ctx.get(), codec, nullptr); ret < 0)
            throw MediaError("Il decoder di " + clips_[c].path + " non si apre: " + ffmpegError(ret));
        return ctx;
    };
    const int videoIndex = findMainStream(input, AVMEDIA_TYPE_VIDEO);
    if (videoIndex < 0)
        throw ClipsNotJoinable("Il file " + clips_[c].path + " non ha video.");
    const AVStream* inV = input->streams[videoIndex];
    CodecContextPtr videoDecoder = openDecoder(videoIndex);
    const int audioIndex = outAudio_ >= 0 ? findMainStream(input, AVMEDIA_TYPE_AUDIO) : -1;
    const bool copyAudio = audioIndex >= 0 && plan_.copyAudio[c];
    CodecContextPtr audioDecoder = audioIndex >= 0 && !copyAudio ? openDecoder(audioIndex) : nullptr;
    AVStream* outA = outAudio_ >= 0 ? output_->streams[outAudio_] : nullptr;
    std::unique_ptr<AudioEncoder> audioEncoder;
    StreamCopy audioOut;  // il flusso audio del file creato (per il silenzio nei tratti senza audio)
    if (outA != nullptr) {
        describeAudio(audioOut, outA->codecpar);
        audioOut.output = outAudio_;
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
    seekBefore(input, videoIndex, range.start, start);

    // --- Video: fotogramma `k` del tratto al tempo range.start + k / fps.
    FramePtr held;  // l'ultimo fotogramma decodificato non oltre il tempo del prossimo fotogramma da scrivere
    int k = 0;
    bool videoDone = false;
    const double half = frameSeconds / 2;
    FramePtr emitted(av_frame_alloc());  // riferimento al fotogramma da scrivere, col suo tempo
    if (!emitted)
        throw MediaError(kOutOfMemory);
    const auto emit = [&](const AVFrame* frame) {
        av_frame_unref(emitted.get());
        if (av_frame_ref(emitted.get(), frame) < 0)
            throw MediaError(kOutOfMemory);
        emitted->pts = base + k;
        video.encoder.encode(emitted.get(), k == 0, videoSink);
        ++k;
        if (progress_ && !progress_(std::clamp((doneLength_ + k * frameSeconds) / totalLength_, 0.0, 1.0)))
            throw ExportCancelled();
    };
    const auto onVideoFrame = [&](AVFrame* frame) {
        if (videoDone)
            return;
        const std::int64_t when = frameTimestamp(frame);
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
    double audioEnd = outputPosition_;
    const int outRate = outA != nullptr ? std::max(1, outA->codecpar->sample_rate) : 1;
    const PacketSink audioSink = outA != nullptr ? audioSinkFor(outRate, &audioOut, audioEnd) : PacketSink{};
    const auto onAudioFrame = [&](AVFrame* frame) {
        if (audioDone)
            return;
        const AVStream* in = input->streams[audioIndex];
        const std::int64_t when = frameTimestamp(frame);
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
        if (!part)
            throw MediaError(kOutOfMemory);
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
        const std::int64_t ts = packetTimestamp(pkt);
        if (ts == AV_NOPTS_VALUE)
            return;
        const double tb = av_q2d(in->time_base);
        const double time = ts * tb - start;
        const double decodeTime = (pkt->dts != AV_NOPTS_VALUE ? pkt->dts : ts) * tb - start;
        if (decodeTime >= range.end + kReadPastOut) {
            audioDone = true;
            return;
        }
        copyAudioPacket(pkt, copy, in, time, start, range, continues, range.start + start - outputPosition_, c, audioEnd);
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
        throw MediaError("Nel tratto da ricodificare di " + clips_[c].path + " non c'è nessun fotogramma.");
    while (k < frames)
        emit(held.get());
    video.encoder.finish(videoSink);
    if (audioEncoder) {
        // Il file non ha audio: silenzio per tutto il tratto.
        if (audioIndex < 0)
            encodeSilence(*audioEncoder, outRate, outA->codecpar->ch_layout.nb_channels,
                          std::llround(range.length() * outRate), audioSink);
        audioEncoder->finish(audioSink);
    }
    for (unsigned i = 0; i < input->nb_streams; ++i)
        input->streams[i]->discard = discarded[i];  // i flussi copiati nelle parti dopo
    return {outputPosition_ + frames * frameSeconds, audioEnd};
}

// Uno spazio vuoto: nero (nel formato del progetto, come i tratti ricodificati: fotogrammi al secondo costanti,
// primo fotogramma chiave, parametri nel flusso) e silenzio (AAC nel formato dell'audio del progetto), dalla
// posizione attuale del file creato. Restituisce dove finiscono video e audio. Uno spazio più corto di mezzo
// fotogramma non lascia niente.
std::pair<double, double> ExportJob::writeBlack(double seconds)
{
    const StreamSignature& format = *plan_.reference.video();
    const AVRational fps = projectFrameRate(format);
    const double frameSeconds = av_q2d(av_inv_q(fps));
    const int frames = static_cast<int>(std::llround(seconds / frameSeconds));
    if (frames <= 0)
        return {outputPosition_, outputPosition_};
    // Tempi in 1/90000 s: il nero parte esattamente dove finisce il pezzo prima (non arrotondato ai fotogrammi).
    constexpr int kTicks = 90000;
    ProjectVideoEncoder video(format, fps, Rational{1, kTicks}, reencode_.quality);
    const PacketSink videoSink = videoSinkFor(video, std::llround(frameSeconds * kTicks), frameSeconds);
    // Il fotogramma nero è sempre lo stesso, cambia solo il tempo.
    const FramePtr black = makeBlackFrame(format.width, format.height);
    for (int k = 0; k < frames; ++k) {
        black->pts = std::llround((outputPosition_ + k * frameSeconds) * kTicks);
        video.encoder.encode(black.get(), k == 0, videoSink);
        if (progress_ && !progress_(std::clamp((doneLength_ + (k + 1) * frameSeconds) / totalLength_, 0.0, 1.0)))
            throw ExportCancelled();
    }
    video.encoder.finish(videoSink);

    // Silenzio, lungo quanto il nero.
    double audioEnd = outputPosition_;
    if (outAudio_ >= 0) {
        const AVCodecParameters* par = output_->streams[outAudio_]->codecpar;
        const int rate = std::max(1, par->sample_rate);
        AudioEncoder silenceEncoder(AudioEncoderSettings{rate, par->ch_layout.nb_channels});
        const PacketSink audioSink = audioSinkFor(rate, nullptr, audioEnd);
        encodeSilence(silenceEncoder, rate, par->ch_layout.nb_channels, std::llround(frames * frameSeconds * rate), audioSink);
        silenceEncoder.finish(audioSink);
    }
    ++result_.blackGaps;
    return {outputPosition_ + frames * frameSeconds, audioEnd};
}


// Un tratto copiato dal file `current` (il `c`-esimo): i pacchetti di tutti i flussi dal fotogramma chiave all'inizio del
// tratto fino alla sua fine, con i tempi spostati nel file creato. Aggiorna outputPosition_ e doneLength_.
void ExportJob::copyRange(InputFile& current, std::size_t c, const Piece& piece)
{
    const TimeRange& range = piece.range;
    AVFormatContext* input = current.ctx.get();
    const double start = current.start;
    // Tutti i tempi del tratto vengono spostati dello stesso valore, per video e audio:
    // tempo nel file creato = tempo nell'originale - range.start + outputPosition.
    const double shiftSeconds = range.start + start - outputPosition_;
    double videoEnd = outputPosition_;  // fine dell'ultimo fotogramma scritto di questo tratto
    double audioEnd = outputPosition_;  // fine dell'audio compresso come uscirà dal decoder

    for (StreamCopy& copy : current.streams) {
        copy.started = false;
        copy.finished = copy.output < 0;
        copy.pending.clear();
    }
    int remaining = clipStreams_;

    // Ci si posiziona sul fotogramma chiave all'inizio del tratto (o quello subito prima).
    if (current.pacingStream >= 0 && remaining > 0)
        seekBefore(input, current.pacingStream, range.start, start);

    while (remaining > 0) {
        const int ret = av_read_frame(input, packet_.get());
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0)
            throw MediaError("Errore durante la lettura del file originale (" + ffmpegError(ret) + ").");

        StreamCopy& copy = current.streams[packet_->stream_index];
        const AVStream* in = input->streams[packet_->stream_index];
        const std::int64_t ts = packetTimestamp(packet_.get());
        if (copy.output < 0 || copy.finished || ts == AV_NOPTS_VALUE) {
            av_packet_unref(packet_.get());
            continue;
        }
        const double tb = av_q2d(in->time_base);
        const double time = ts * tb - start;
        const double decodeTime = (packet_->dts != AV_NOPTS_VALUE ? packet_->dts : ts) * tb - start;

        if (decodeTime >= range.end + kReadPastOut) {
            // Questo flusso ha finito il tratto: il video tenuto da parte oltre la fine non serve.
            copy.finished = true;
            copy.pending.clear();
            --remaining;
            av_packet_unref(packet_.get());
            continue;
        }

        if (copy.video) {
            if (!copy.started) {
                if ((packet_->flags & AV_PKT_FLAG_KEY) == 0 || time < range.start - kTimeEpsilon) {
                    av_packet_unref(packet_.get());
                    continue;
                }
                copy.started = true;
            }
            if (time < range.start - kTimeEpsilon) {
                // B-frame che precedono il fotogramma chiave (GOP aperto): senza il GOP prima non si
                // decodificano.
                av_packet_unref(packet_.get());
                continue;
            }
            if (time >= range.end - kTimeEpsilon) {
                // Oltre la fine: lo teniamo da parte e lo scriviamo solo se poi arriva un fotogramma
                // prima della fine (che potrebbe usarlo come riferimento).
                PacketPtr held(av_packet_alloc());
                av_packet_move_ref(held.get(), packet_.get());
                copy.pending.push_back(std::move(held));
                continue;
            }
            for (PacketPtr& held : copy.pending)
                writeCopied(held.get(), copy, in, shiftSeconds, c, videoEnd, audioEnd);
            copy.pending.clear();
            writeCopied(packet_.get(), copy, in, shiftSeconds, c, videoEnd, audioEnd);
        } else if (!copyAudioPacket(packet_.get(), copy, in, time, start, range, piece.continues, shiftSeconds, c,
                                    audioEnd)) {
            av_packet_unref(packet_.get());
            continue;
        }
        if (progress_) {
            const double inRange = std::clamp(time - range.start, 0.0, range.length());
            if (!progress_(std::clamp((doneLength_ + inRange) / totalLength_, 0.0, 1.0)))
                throw ExportCancelled();
        }
    }

    // Il tratto successivo comincia dove finisce questo: alla sua durata, o più in là se è stato scritto
    // qualche fotogramma di riferimento oltre la fine o se l'ultimo blocco audio decodificato va oltre
    // (pochi millesimi): così video e audio ripartono insieme e l'audio non si sovrappone.
    outputPosition_ = std::max({outputPosition_ + range.length(), videoEnd, piece.continues ? 0.0 : audioEnd});
    doneLength_ += range.length();
}

ExportResult ExportJob::run()
{
    try {
        if ((output_->oformat->flags & AVFMT_NOFILE) == 0) {
            if (const int ret = avio_open(&output_->pb, outputPath_.c_str(), AVIO_FLAG_WRITE); ret < 0)
                throw MediaError("Impossibile creare il file di destinazione (" + ffmpegError(ret) + "): " + outputPath_);
        }
        if (const int ret = avformat_write_header(output_.get(), nullptr); ret < 0)
            throw MediaError("Il formato di destinazione non accetta questi flussi (" + ffmpegError(ret) + ").");

        packet_.reset(av_packet_alloc());
        if (!packet_)
            throw MediaError(kOutOfMemory);

        // Per ogni flusso del file creato: ultimo tempo di decodifica scritto (devono sempre crescere).
        lastDts_.assign(streamCount_, std::numeric_limits<std::int64_t>::min());

        for (std::size_t c = 0; c < clips_.size(); ++c) {
            InputFile current = c == 0 ? std::move(first_) : openClip(clips_[c].path, !replaced_);
            if (!plan_.reencode[c] && current.copied != clipStreams_ && !plan_.reencode.front())
                throw MediaError("Il file " + clips_[c].path + " ha un numero di flussi diverso dal primo.");

            for (std::size_t r = 0; r < clips_[c].ranges.size(); ++r) {
                // Lo spazio vuoto prima del tratto: nero e silenzio, oppure (chiuso) solo il tempo del montaggio che passa
                // (i tratti senza audio sono nei tempi del montaggio, spazi compresi).
                if (const double gap = gapBefore(clips_[c], r); gap > kTimeEpsilon) {
                    if (plan_.blackGaps && outVideo_ >= 0) {
                        const auto [videoEnd, audioEnd] = writeBlack(gap);
                        outputPosition_ = std::max({outputPosition_, videoEnd, audioEnd});
                    }
                    doneLength_ += gap;
                }
                for (const Piece& piece : plan_.pieces[c][r]) {
                    const TimeRange& range = piece.range;
                    if (piece.reencode) {
                        const auto [videoEnd, audioEnd] = reencodeRange(current, range, c, piece.continues);
                        if (plan_.reencode[c])
                            ++result_.reencodedSegments;
                        else
                            ++result_.preciseCuts;
                        // Dentro lo stesso tratto l'audio continua: la parte dopo riparte esattamente da qui.
                        outputPosition_ = std::max({outputPosition_ + range.length(), videoEnd, piece.continues ? 0.0 : audioEnd});
                        doneLength_ += range.length();
                        continue;
                    }
                    copyRange(current, c, piece);
                }
            }
        }
        // Il resto della traccia sostitutiva, fino alla fine del file creato.
        pumpReplacement(std::numeric_limits<double>::max(), outputPosition_);

        if (const int ret = av_write_trailer(output_.get()); ret < 0)
            throw MediaError("Errore nel completare il file (" + ffmpegError(ret) + ").");
        if (progress_)
            progress_(1.0);

        result_.durationSeconds = firstTime_ == std::numeric_limits<double>::max() ? 0.0 : lastEnd_ - firstTime_;
        result_.streamCount = streamCount_;
        result_.segmentCount = rangeCount_;
        result_.clipCount = static_cast<int>(clips_.size());
        result_.parametersInStream = plan_.parametersInStream;
        return result_;
    } catch (...) {
        removePartial();
        throw;
    }
}

}  // namespace segment
}  // namespace editor::core
