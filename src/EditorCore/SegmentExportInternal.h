#pragma once

// Parti interne dell'esportazione dei tratti, condivise da SegmentExport.cpp e SegmentExportJob.cpp.

#include "EditorCore/SegmentExport.h"

#include "EditorCore/Compatibility.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/MixExport.h"
#include "EditorCore/NalUnits.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Speed.h"
#include "EditorCore/LayerFrames.h"
#include "EditorCore/FrameCompositor.h"
#include "EditorCore/GpuDevice.h"
#include "EditorCore/TextFrame.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
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
#include <libavutil/hwcontext.h>
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
void replacePacketData(AVPacket* pkt, const std::uint8_t* data, std::size_t size);

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
bool rewriteNals(AVPacket* pkt, int fromSize, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix);

// Parametri di codifica (SPS/PPS, per HEVC anche VPS) dai "dati extra", in avcC/hvcC o in Annex B (come li danno
// gli encoder).
std::optional<ParameterSets> parameterNalsOf(AVCodecID codec, const std::vector<std::uint8_t>& extra);

// "Dati extra" avcC (lunghezze su 4 byte) da SPS e PPS H.264 senza codici di inizio; vuoto se non ce n'è nessuno.
std::vector<std::uint8_t> buildAvcC(const ParameterSets& sets);

// "Dati extra" hvcC (lunghezze su 4 byte) da VPS, SPS e PPS HEVC senza codici di inizio; vuoto se manca uno dei tre.
std::vector<std::uint8_t> buildHvcC(const ParameterSets& sets);

// Pacchetto dell'encoder (Annex B, o già con le lunghezze su 4 byte) con le lunghezze su `toSize` byte e, davanti, i
// NAL di `prefix`.
bool encodedToLengthPrefixed(AVPacket* pkt, int toSize, const std::vector<std::vector<std::uint8_t>>& prefix);

// get_format del decoder con la GPU: il formato D3D11 (in `opaque`) se il decoder lo offre, altrimenti il primo software.
inline AVPixelFormat pickHardwareFormat(AVCodecContext* context, const AVPixelFormat* formats)
{
    return chooseDecoderFormat(formats, static_cast<AVPixelFormat>(reinterpret_cast<std::intptr_t>(context->opaque)));
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
    // Dentro una transizione (indice in ReencodeOptions::transitions; -1 = no): il video è la fusione dei due pezzi.
    // `transitionEnding` = è la parte che finisce nella giunzione (il tratto prima), altrimenti quella che comincia.
    int transition = -1;
    bool transitionEnding = false;
    // Ricodificata perché il tratto ha una trasformazione (scala, posizione...): `transform` si applica al fotogramma
    // prima delle scritte.
    bool transformed = false;
    Transform transform;
    // Fotogrammi chiave della trasformazione (tempi nel file): a ogni fotogramma si valuta `transform` + `animation`.
    TransformAnimation animation;
    // Velocità e verso del tratto (vedi Speed.h): `range` è nel file, nel montaggio il pezzo dura range.length() / speed.
    double speed = 1.0;
    bool reverse = false;
    bool sped() const { return speed != 1.0 || reverse; }
    double montageLength() const { return range.length() / speed; }
};

// Taglio preciso di un tratto copiato (vedi exportClips): ricodificati i bordi fra IN e il primo fotogramma chiave e, con
// i B-frame, fra l'ultimo fotogramma chiave e OUT; copiato il resto. Tutto ricodificato se dentro non c'è un fotogramma
// chiave da cui copiare.
std::vector<Piece> splitForPreciseCut(const TimeRange& range, const std::vector<double>& keyframes, bool bFrames);

// I momenti del montaggio (tempi della sequenza) in cui si vede almeno una scritta, in ordine e senza sovrapposizioni.
std::vector<TimeRange> textCoverage(const TextTracks* texts);

// Un tratto (il numero `index` del piano) diviso nelle parti dentro una transizione e nelle altre: l'ultima parte del tratto
// prima di una giunzione e la prima di quello dopo. `transition` = -1 per le altre parti.
struct TransitionPart
{
    TimeRange range;
    int transition = -1;
    bool ending = false;
};

std::vector<TransitionPart> splitByTransition(const TimeRange& range, int index, const std::vector<ExportTransition>* transitions);

// Un tratto del file (`range`, che nella sequenza parte da `sequenceStart`) diviso nelle parti sotto una scritta (true) e
// nelle altre (false), in ordine. Una sola parte false se non ci sono scritte.
std::vector<std::pair<TimeRange, bool>> splitByText(const TimeRange& range, double sequenceStart,
                                                           const std::vector<TimeRange>& coverage);

// Che cosa succede a ogni file della lista all'esportazione.
struct PlanAnalysis
{
    std::vector<MediaSignature> signatures;     // per file della lista (vuoto: un solo file, si copia com'è)
    std::vector<bool> reencode;                 // per file
    std::vector<bool> copyAudio;                // per file: l'audio si copia (uguale a quello del formato)
    std::vector<std::vector<std::vector<Piece>>> pieces;  // per file e per tratto: le parti, copiate o ricodificate
    std::string impreciseCuts;                  // vedi ExportPreview
    bool blackGaps = false;                     // gli spazi vuoti diventano nero e silenzio
    bool blackTail = false;                     // dopo l'ultimo pezzo: nero e silenzio fino a ReencodeOptions::tailSeconds
    std::string closedGaps;                     // vedi ExportPreview
    ProjectFormat reference;                    // il formato da rispettare
    bool parametersInStream = false;            // vedi ExportResult
    std::vector<ParameterSets> clipParameters;  // per file copiato, se parametersInStream
    std::string problem;                        // perché non si può esportare; vuoto = si può
    bool videoChanged = false;                  // il video ha un codec, una misura o dei fotogrammi/s diversi dai file
};

// Spazio vuoto prima del tratto `r` di `clip` (0 se il piano non ne ha).
inline double gapBefore(const ClipRanges& clip, std::size_t r)
{
    return r < clip.gapsBefore.size() ? std::max(0.0, clip.gapsBefore[r]) : 0.0;
}

// Trasformazione del tratto `r` di `clip` (nessuna se il piano non ne ha).
inline Transform transformOf(const ClipRanges& clip, std::size_t r)
{
    return r < clip.transforms.size() ? normalized(clip.transforms[r]) : Transform{};
}

// Fotogrammi chiave della trasformazione del tratto `r` di `clip` (nessuno se il piano non ne ha).
inline TransformAnimation animationOf(const ClipRanges& clip, std::size_t r)
{
    return r < clip.animations.size() ? clip.animations[r] : TransformAnimation{};
}

// Velocità del tratto `r` di `clip` (1 se il piano non la dice) e verso (false = in avanti).
inline double speedOf(const ClipRanges& clip, std::size_t r)
{
    return r < clip.speeds.size() ? clampSpeed(clip.speeds[r]) : 1.0;
}
inline bool reverseOf(const ClipRanges& clip, std::size_t r)
{
    return r < clip.reverses.size() && clip.reverses[r];
}
// Quanto dura il tratto `r` di `clip` nel montaggio (secondi).
inline double montageLength(const ClipRanges& clip, std::size_t r)
{
    return clip.ranges[r].length() / speedOf(clip, r);
}

PlanAnalysis analyzePlan(const std::vector<ClipRanges>& clips, const ProjectFormat* format, bool replaced, bool reencodeAll,
                         bool closeGaps, const TextTracks* texts = nullptr,
                         const std::vector<ExportTransition>* transitions = nullptr,
                         std::optional<EncoderBackend> backend = std::nullopt, const VideoChange* change = nullptr,
                         double tailSeconds = 0.0);

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
bool trimPcm(AVPacket* pkt, const StreamCopy& copy, double time, double tb, const TimeRange& range);

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
void setExtradata(AVCodecParameters* par, const std::uint8_t* data, std::size_t size);

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

// Finché c'è un encoder che prende fotogrammi D3D11, il compositore scrive i fotogrammi finiti direttamente nelle sue texture.
struct HardwareOutputScope
{
    HardwareOutputScope(IFrameCompositor& compositor, AVBufferRef* frames)
        : compositor_(compositor)
    {
        compositor_.setHardwareOutput(frames);
    }
    ~HardwareOutputScope() { compositor_.setHardwareOutput(nullptr); }
    HardwareOutputScope(const HardwareOutputScope&) = delete;
    HardwareOutputScope& operator=(const HardwareOutputScope&) = delete;

private:
    IFrameCompositor& compositor_;
};

// Encoder video nel formato del progetto (H.264 o HEVC) con i suoi parametri di codifica già letti, per i tratti da
// ricodificare e per il nero.
struct ProjectVideoEncoder
{
    ProjectVideoEncoder(const StreamSignature& format, AVRational fps, Rational timeBase, const ReencodeOptions& options,
                        AVBufferRef* d3d11Device = nullptr)
        : encoder(settingsFor(format, fps, timeBase, options, d3d11Device))
    {
        AVCodecParameters* par = avcodec_parameters_alloc();
        if (par == nullptr)
            throw MediaError(kOutOfMemory);
        encoder.copyParameters(par);
        extradata.assign(par->extradata, par->extradata + std::max(0, par->extradata_size));
        const AVCodecID par_codec = par->codec_id;
        // AV1: niente NAL da ripetere, i pacchetti (OBU) vanno nel file così come sono; la sequenza è nei dati extra.
        lengthPrefixed = par->codec_id != AV_CODEC_ID_AV1;
        const auto sets = lengthPrefixed ? parameterNalsOf(par->codec_id, extradata) : std::optional<ParameterSets>(ParameterSets{});
        avcodec_parameters_free(&par);
        if (!sets)
            throw MediaError("L'encoder " + encoder.info().ffmpegName + " non ha dato i parametri di codifica.");
        parameters = *sets;
        // H.264 con i parametri in Annex B (come li dà l'encoder): il flusso del file vuole l'avcC.
        if (par_codec == AV_CODEC_ID_H264 && (extradata.empty() || extradata[0] != 1)) {
            if (std::vector<std::uint8_t> avcc = buildAvcC(parameters); !avcc.empty())
                extradata = std::move(avcc);
        }
        // Lo stesso per HEVC: hvcC invece dei VPS/SPS/PPS in Annex B (MKV e MP4 vogliono quello, con i pacchetti a lunghezze).
        if (par_codec == AV_CODEC_ID_HEVC && (extradata.empty() || extradata[0] != 1)) {
            if (std::vector<std::uint8_t> hvcc = buildHvcC(parameters); !hvcc.empty())
                extradata = std::move(hvcc);
        }
    }

    VideoEncoder encoder;
    ParameterSets parameters;
    bool lengthPrefixed = true;           // H.264 / HEVC: NAL con la lunghezza davanti (AV1: pacchetti com'è)
    std::vector<std::uint8_t> extradata;  // i parametri di codifica come li dà l'encoder (per il flusso del file)

private:
    static VideoEncoderSettings settingsFor(const StreamSignature& format, AVRational fps, Rational timeBase, const ReencodeOptions& options,
                                            AVBufferRef* d3d11Device)
    {
        VideoEncoderSettings settings;
        settings.codec = videoCodecFromName(format.codec).value_or(VideoCodec::H264);
        settings.width = format.width;
        settings.height = format.height;
        settings.frameRate = {fps.num, fps.den};
        settings.timeBase = timeBase;
        settings.quality = options.quality;
        settings.backend = options.backend;
        settings.preset = options.preset;
        settings.cq = options.cq;
        settings.d3d11Device = d3d11Device;
        return settings;
    }
};

// Il formato dei pixel in cui si fondono scritte e transizioni: quello del progetto (così un progetto a 10 bit non
// perde bit nei tratti con una scritta); se è sconosciuto o non si può produrre, YUV 4:2:0 a 8 bit.
inline AVPixelFormat blendPixelFormat(const StreamSignature& format)
{
    const AVPixelFormat wanted = av_get_pix_fmt(format.pixelFormat.c_str());
    if (wanted == AV_PIX_FMT_NONE || (av_pix_fmt_desc_get(wanted)->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0 ||
        sws_isSupportedOutput(wanted) == 0)
        return AV_PIX_FMT_YUV420P;
    return wanted;
}

// Fotogramma nero (gamma TV: luminanza 16, colore 128) della misura data.
FramePtr makeBlackFrame(int width, int height, AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P);

// Codifica `samples` campioni di silenzio (a blocchi di 1024), con un solo fotogramma riusato per tutti.
void encodeSilence(AudioEncoder& encoder, int sampleRate, int channels, std::int64_t samples, const PacketSink& sink);

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
void describeAudio(StreamCopy& copy, const AVCodecParameters* par);

// withAudio = false: l'audio del file non si copia (al suo posto c'è una traccia sostitutiva).
InputFile openClip(const std::string& path, bool withAudio);

// Un blocco di silenzio per un flusso audio compresso, codificato con lo stesso codec e gli stessi parametri
// (frequenza, canali, profilo): il decoder dell'originale lo legge come il resto del flusso. Codificare il silenzio
// non tocca l'audio originale, che resta copiato così com'è. std::nullopt se qui non si sa fare (codec senza encoder,
// o parametri che l'encoder non riproduce).
std::optional<std::vector<std::uint8_t>> encodeSilentFrame(const AVCodecParameters* par);

// AAC con l'intestazione ADTS in ogni pacchetto (es. nei file .ts): la si aggiunge anche al blocco di silenzio.
std::vector<std::uint8_t> withAdtsHeader(const std::vector<std::uint8_t>& raw, const AVCodecParameters* par);

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
bool applyMute(AVPacket* pkt, StreamCopy& copy, const AVCodecParameters* par, double seqStart, double seqEnd,
               const std::vector<TimeRange>& muted, MuteMethod method, ExportResult& stats);

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
bool canMux(const AVOutputFormat* format, const std::string& extension, AVFormatContext* input, int streamIndex);

// Come replacementAudioProblem, con il file audio già aperto in `ctx` e il suo flusso audio principale `index`
// (-1 se non c'è). Legge qualche pacchetto: prima di usare `ctx` per copiare l'audio bisogna riposizionarsi.
std::string replacementProblem(AVFormatContext* ctx, int index, const std::string& audioPath,
                               const std::string& outputPath);

// La codifica del video in un thread suo: la composizione dei fotogrammi (sul thread di chi chiama) e la codifica vanno in parallelo.
// push() mette un fotogramma in coda (ne tiene un riferimento: il suo contenuto non deve più cambiare) e aspetta solo se la coda è
// piena; il thread chiama `encode(frame, key)` nell'ordine. Un errore del thread (anche ExportCancelled) si rilancia al push() o
// al drain() dopo. Il distruttore ferma il thread (scarta i fotogrammi in coda) senza lanciare.
class AsyncFrameEncoder
{
public:
    using EncodeFn = std::function<void(const AVFrame* frame, bool key)>;
    explicit AsyncFrameEncoder(EncodeFn encode, std::size_t depth = 3)
        : encode_(std::move(encode))
        , depth_(std::max<std::size_t>(1, depth))
        , worker_([this] { run(); })
    {
    }
    ~AsyncFrameEncoder()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            queue_.clear();
        }
        wake_.notify_all();
        space_.notify_all();
        worker_.join();
    }
    AsyncFrameEncoder(const AsyncFrameEncoder&) = delete;
    AsyncFrameEncoder& operator=(const AsyncFrameEncoder&) = delete;

    void push(const AVFrame* frame, bool key)
    {
        FramePtr copy(av_frame_clone(frame));
        if (!copy)
            throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [&] { return queue_.size() < depth_ || error_; });
        rethrow(lock);
        queue_.emplace_back(std::move(copy), key);
        wake_.notify_one();
    }
    // Quanto ha lavorato il thread (secondi di orologio dentro encode); da leggere dopo drain().
    double busySeconds()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return busySeconds_;
    }
    // Aspetta che tutti i fotogrammi in coda siano codificati (prima di finish() dell'encoder o di usarlo da un altro thread).
    void drain()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [&] { return (queue_.empty() && !busy_) || error_; });
        rethrow(lock);
    }

private:
    void rethrow(std::unique_lock<std::mutex>&)
    {
        if (error_) {
            const std::exception_ptr error = error_;
            error_ = nullptr;
            std::rethrow_exception(error);
        }
    }
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            auto item = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            std::exception_ptr failure;
            const auto began = std::chrono::steady_clock::now();
            try {
                encode_(item.first.get(), item.second);
            } catch (...) {
                failure = std::current_exception();
            }
            item.first.reset();
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            lock.lock();
            busySeconds_ += took;
            busy_ = false;
            if (failure) {
                error_ = failure;
                queue_.clear();
            }
            space_.notify_all();
        }
    }

    EncodeFn encode_;
    std::size_t depth_;
    std::mutex mutex_;
    std::condition_variable wake_;   // c'è lavoro (o ci si ferma)
    std::condition_variable space_;  // c'è posto in coda, la coda è vuota, o c'è un errore
    std::deque<std::pair<FramePtr, bool>> queue_;
    bool busy_ = false;
    double busySeconds_ = 0.0;
    bool stop_ = false;
    std::exception_ptr error_;
    std::thread worker_;  // ultimo: parte quando il resto è pronto
};

// Lettura e decodifica del video in un thread suo: `produce(channel)` gira nel thread e dà i fotogrammi con channel.push(); chi
// consuma li prende con pop() (nullptr = finiti). Un errore del produttore (eccezione) si rilancia al pop() o al join().
// Il distruttore ferma il produttore (il suo push() torna false) e lo aspetta, senza lanciare.
class DecodeThread
{
public:
    class Channel
    {
    public:
        explicit Channel(std::size_t depth) : depth_(std::max<std::size_t>(1, depth)) {}
        // Dal produttore: false se il consumatore non vuole più fotogrammi (il fotogramma si scarta).
        bool push(FramePtr frame)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            space_.wait(lock, [&] { return queue_.size() < depth_ || cancelled_; });
            if (cancelled_)
                return false;
            queue_.push_back(std::move(frame));
            ready_.notify_one();
            return true;
        }
        // Dal consumatore: il prossimo fotogramma; nullptr quando il produttore ha finito. Rilancia il suo errore.
        FramePtr pop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [&] { return !queue_.empty() || finished_; });
            if (queue_.empty()) {
                if (error_) {
                    const std::exception_ptr error = error_;
                    error_ = nullptr;
                    std::rethrow_exception(error);
                }
                return nullptr;
            }
            FramePtr frame = std::move(queue_.front());
            queue_.pop_front();
            space_.notify_one();
            return frame;
        }
        // Il consumatore non vuole più fotogrammi: si svuota la coda e i push() tornano false.
        void cancel()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            queue_.clear();
            space_.notify_all();
        }
        bool cancelled()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return cancelled_;
        }
        void finish(std::exception_ptr error)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
            error_ = error;
            ready_.notify_all();
        }

    private:
        std::size_t depth_;
        std::mutex mutex_;
        std::condition_variable ready_;
        std::condition_variable space_;
        std::deque<FramePtr> queue_;
        bool cancelled_ = false;
        bool finished_ = false;
        std::exception_ptr error_;
    };

    DecodeThread(std::size_t depth, std::function<void(Channel&)> produce)
        : channel_(depth)
        , worker_([this, produce = std::move(produce)] {
            std::exception_ptr failure;
            try {
                produce(channel_);
            } catch (...) {
                failure = std::current_exception();
            }
            channel_.finish(failure);
        })
    {
    }
    ~DecodeThread()
    {
        channel_.cancel();
        if (worker_.joinable())
            worker_.join();
    }
    DecodeThread(const DecodeThread&) = delete;
    DecodeThread& operator=(const DecodeThread&) = delete;

    FramePtr pop() { return channel_.pop(); }
    // Il consumatore ha finito coi fotogrammi: il produttore continua solo per quello che gli resta da fare (es. l'audio). Poi
    // join() lo aspetta e rilancia il suo errore, se c'è.
    void stopFrames() { channel_.cancel(); }
    void join()
    {
        if (worker_.joinable())
            worker_.join();
        // Un errore non ancora visto dal pop() (il produttore ha fallito dopo l'ultimo fotogramma).
        while (channel_.pop() != nullptr) {
        }
    }

private:
    Channel channel_;
    std::thread worker_;  // ultimo: parte quando il resto è pronto
};

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
    void ensureGpuDevice();
    void noteEncoder(const ProjectVideoEncoder& video);
    AVBufferRef* gpuFfmpegDevice();  // il dispositivo D3D11 condiviso di FFmpeg, o nullptr
    PacketSink audioSinkFor(int rate, StreamCopy* muting, double& audioEnd);
    std::pair<double, double> reencodeRange(InputFile& current, const TimeRange& range, std::size_t c, const Piece& piece);
    std::pair<double, double> writeBlack(double seconds);
    // Le scritte che si vedono al tempo `sequenceTime` del montaggio, già disegnate alla misura del fotogramma del progetto
    // (dal livello più basso al più alto); vuoto se non ce ne sono.
    std::vector<BlendedText> textsAt(double sequenceTime);
    // I pezzi dei livelli sopra V1 che si vedono al tempo `sequenceTime`: li passa al blender, che li compone sopra il
    // fotogramma della prossima chiamata. true se ce n'è almeno uno (e quindi il fotogramma deve passare dal blender).
    bool prepareLayers(double sequenceTime);
    // Il fotogramma con le scritte sopra (alla misura del progetto), da dare all'encoder al posto di `frame`.
    const AVFrame* withText(const AVFrame* frame, const std::vector<BlendedText>& texts);
    // Il compositore dei fotogrammi (GPU o CPU, vedi ReencodeOptions::compositor), creato al primo uso.
    IFrameCompositor& compositor();

    const std::vector<ClipRanges>& clips_;
    const std::string& outputPath_;
    const AudioEdit& audio_;
    const ExportProgress& progress_;
    const MuteMethod muteMethod_;
    const ReencodeOptions& reencode_;

    double totalLength_ = 0.0;  // del montaggio, spazi vuoti compresi
    int rangeCount_ = 0;
    const bool replaced_;  // c'è una traccia sostitutiva
    const bool noClipAudio_;  // l'audio dei file non entra nel file creato (traccia sostitutiva, o senza audio)
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
    std::vector<TextImage> transformedTexts_;  // le scritte animate del fotogramma in corso (vedi textsAt)
    // Per id di scritta: l'immagine disegnata una volta sola e, se la scritta ha una trasformazione fissa che la sposta, la
    // stessa già trasformata (anche questa una volta sola). Si liberano quando la scritta non si vede più.
    struct CachedText
    {
        TextImage drawn;
        std::optional<TextImage> transformed;
    };
    std::map<int, CachedText> textImages_;
    std::unique_ptr<IFrameCompositor> blender_;
    std::unique_ptr<LayerFrameSource> layerSource_;  // i fotogrammi dei pezzi dei livelli (vedi ReencodeOptions::layerFiles)
    std::vector<LayerFrameSource::Visible> layerFrames_;  // quelli del fotogramma in corso, tenuti vivi finché il blender li usa
    std::shared_ptr<AVBufferRef> hwDevice_;  // il dispositivo D3D11 per decodificare con la GPU, creato una volta sola
    bool hwDeviceTried_ = false;
    bool gpuDeviceTried_ = false;
    std::string encoderNote_;  // perché l'encoder non prende texture D3D11 (vuoto se le prende)
    std::shared_ptr<GpuDevice> gpuDevice_;  // se c'è, hwDevice_ è questo stesso dispositivo: i fotogrammi decodificati li legge il compositore

    std::mutex muxMutex_;  // i pacchetti dei due flussi (video dal thread dell'encoder, audio dall'altro) si scrivono uno alla volta
    std::unique_ptr<TransitionRenderer> transitionRenderer_;  // per le parti dentro una transizione
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
