#pragma once

#include "EditorCore/MediaError.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Tipi di FFmpeg usati solo per puntatore (le definizioni sono negli header di FFmpeg).
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;

namespace editor::core {

// Codifica (ricodifica) di video e audio, per le parti del montaggio che non si possono solo copiare.
//
// Il video si codifica con la scheda video quando si può: NVIDIA NVENC, poi Intel QSV e AMD AMF (se la build di FFmpeg
// li ha), poi Media Foundation di Windows (che usa la scheda video o, se non c'è, il codificatore software di Windows).
// Quali encoder funzionano davvero su questo PC si scopre provandoli: ognuno codifica un fotogramma (vedi
// availableVideoEncoders). L'audio si codifica in AAC con l'encoder di FFmpeg.

enum class VideoCodec
{
    H264,
    Hevc,
    Av1,
};

enum class EncoderBackend
{
    Nvenc,            // NVIDIA
    Qsv,              // Intel Quick Sync
    Amf,              // AMD
    MediaFoundation,  // Windows (scheda video o software)
    Software,         // sul processore (x264, x265, SVT-AV1), se la build di FFmpeg li include
};

// Qualità costante: la quantità di dati segue la complessità delle immagini.
enum class QualityPreset
{
    High,    // "Alta": quasi indistinguibile dall'originale
    Medium,  // "Media": file più piccoli
};

struct Rational
{
    int num = 0;
    int den = 1;
};

// Un encoder video provato e funzionante.
struct VideoEncoderInfo
{
    VideoCodec codec = VideoCodec::H264;
    EncoderBackend backend = EncoderBackend::MediaFoundation;
    std::string ffmpegName;  // es. "h264_nvenc"
};

std::string codecName(VideoCodec codec);        // "H.264", "HEVC", "AV1"
std::string backendName(EncoderBackend backend);  // "NVIDIA NVENC", "Media Foundation"...
// es. "H.264 - NVIDIA NVENC (h264_nvenc)"
std::string describe(const VideoEncoderInfo& info);

// Gli encoder video che funzionano su questo PC, in ordine di preferenza (per ogni codec: NVENC, QSV, AMF, Media
// Foundation, software). La prova (un fotogramma codificato con ogni encoder) si fa la prima volta che si chiama, anche da più
// thread insieme, e il risultato resta in memoria: le volte dopo è immediata. Per usarne uno solo (preferredVideoEncoder,
// VideoEncoder) si provano solo gli encoder di quel codec: AV1 non lo prova l'esportazione, solo questo elenco.
const std::vector<VideoEncoderInfo>& availableVideoEncoders();
// Il primo encoder funzionante per il codec, se c'è.
std::optional<VideoEncoderInfo> preferredVideoEncoder(VideoCodec codec);
// Il primo encoder funzionante per il codec con questo tipo (senza `backend`: come preferredVideoEncoder).
std::optional<VideoEncoderInfo> findVideoEncoder(VideoCodec codec, std::optional<EncoderBackend> backend);

struct VideoEncoderSettings
{
    VideoCodec codec = VideoCodec::H264;
    int width = 0;
    int height = 0;
    Rational frameRate{25, 1};
    // Unità dei tempi (pts) dei fotogrammi dati all'encoder e dei pacchetti prodotti; {0, 1} = 1 / frameRate.
    Rational timeBase{0, 1};
    Rational sampleAspectRatio{1, 1};
    QualityPreset quality = QualityPreset::High;
    // Distanza massima fra due fotogrammi chiave, in fotogrammi (0 = 2 secondi). I fotogrammi chiave chiesti con
    // forceKeyframe si aggiungono a questi.
    int gopFrames = 0;
    // Parametri del flusso (per H.264: SPS e PPS) ripetuti in ogni fotogramma chiave invece che una volta sola nei
    // "dati extra" del contenitore. Serve quando i pacchetti codificati vanno uniti a pacchetti copiati da un file.
    bool inBandParameters = false;
    // Un encoder preciso invece del primo funzionante (es. per le prove).
    std::optional<EncoderBackend> backend;
    // Fotogrammi di riferimento (0 = quelli scelti dall'encoder). Non tutti gli encoder lo rispettano.
    int referenceFrames = 0;
    // B-frame fra un fotogramma e l'altro (0 = nessuno: l'ordine dei pacchetti è quello dei fotogrammi, più semplice da
    // unire ai pezzi copiati). Per le prove con file che li hanno.
    int bFrames = 0;
    // Preset di velocità/qualità dell'encoder (uno dei valori di VideoEncoderTuning::presets; vuoto = quello di default
    // dell'encoder) e qualità costante (CQ) da 0..51 (meno = migliore; 0 = quella di `quality`). Se valorizzati hanno la
    // precedenza su `quality`.
    std::string preset;
    int cq = 0;
};

// Un preset di un encoder: il valore da passargli e il nome da mostrare.
struct EncoderPreset
{
    std::string value;  // es. "p5"
    std::string label;  // es. "p5 (bilanciato)"
};

// Che cosa si può regolare in un encoder video: i preset e la scala della qualità costante (CQ, meno = migliore).
struct VideoEncoderTuning
{
    std::vector<EncoderPreset> presets;  // dal più veloce al più lento; vuoto = l'encoder non ha preset
    std::string defaultPreset;           // valore del preset di default (uno di `presets`, o vuoto)
    int cqMin = 0;
    int cqMax = 51;
    int cqHigh = 19;    // il CQ della qualità "Alta" per questo encoder
    int cqMedium = 25;  // il CQ della qualità "Media"
};

// Preset e scala del CQ di un encoder (non apre l'encoder).
VideoEncoderTuning videoEncoderTuning(const VideoEncoderInfo& info);

// Se il contenitore dedotto dall'estensione ("mp4", ".mkv", senza distinzione fra maiuscole) può contenere il codec.
bool videoCodecFitsContainer(VideoCodec codec, const std::string& extension);

// Riceve ogni pacchetto prodotto. Il pacchetto appartiene all'encoder: va usato (o copiato) durante la chiamata. I tempi
// sono nell'unità timeBase() dell'encoder.
using PacketSink = std::function<void(AVPacket* packet)>;

class VideoEncoder
{
public:
    // Lancia MediaError (in italiano) se nessun encoder adatto funziona.
    explicit VideoEncoder(const VideoEncoderSettings& settings);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    const VideoEncoderInfo& info() const;
    Rational timeBase() const;
    // Codec, misure e dati extra del flusso, per il contenitore (dopo l'apertura sono già quelli definitivi).
    void copyParameters(AVCodecParameters* parameters) const;

    // Codifica un fotogramma decodificato, con pts nell'unità timeBase(). Qualsiasi formato dei pixel e misura
    // (anche un fotogramma della scheda video): se serve viene convertito e ridimensionato. forceKeyframe = questo
    // fotogramma sarà un fotogramma chiave (IDR per H.264 e HEVC), da cui si può iniziare a decodificare.
    void encode(const AVFrame* frame, bool forceKeyframe, const PacketSink& sink);
    // Fine: escono i pacchetti rimasti dentro l'encoder. Dopo non si può più codificare.
    void finish(const PacketSink& sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Codec audio di uscita. L'encoder c'è solo se questa versione di FFmpeg lo include (vedi availableAudioCodecs).
enum class AudioCodec
{
    Aac,
    Mp3,
    Opus,
    Ac3,
    Flac,  // senza perdita: nessun bitrate
};

std::string audioCodecName(AudioCodec codec);  // "AAC", "MP3", "Opus", "AC-3", "FLAC"
// I codec audio che si possono codificare con questa versione di FFmpeg (in questo ordine: AAC per primo).
const std::vector<AudioCodec>& availableAudioCodecs();
// Se il contenitore dedotto dall'estensione può contenere il codec (senza opzioni sperimentali).
bool audioCodecFitsContainer(AudioCodec codec, const std::string& extension);
// I bitrate da proporre, in kb/s (vuoto per FLAC) e quello di default.
std::vector<int> audioBitRatesKbps(AudioCodec codec);
int defaultAudioBitRateKbps(AudioCodec codec);

struct AudioEncoderSettings
{
    int sampleRate = 48000;  // se il codec non la ammette si usa la più vicina fra quelle ammesse (vedi timeBase())
    int channels = 2;
    int bitRate = 0;  // bit al secondo (0 = quello di default del codec: 192 kb/s, AC-3 384 kb/s)
    AudioCodec codec = AudioCodec::Aac;
};

// Audio in uno dei codec di AudioCodec (di default AAC). I tempi dei pacchetti sono in 1/frequenza dell'encoder (timeBase()), a partire da 0: i campioni dati a encode() sono considerati uno dopo
// l'altro, senza buchi (convertiti al formato, alla frequenza e ai canali dell'encoder se diversi).
class AudioEncoder
{
public:
    explicit AudioEncoder(const AudioEncoderSettings& settings);
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    Rational timeBase() const;
    void copyParameters(AVCodecParameters* parameters) const;
    void encode(const AVFrame* frame, const PacketSink& sink);
    void finish(const PacketSink& sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace editor::core
