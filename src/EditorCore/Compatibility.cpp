#include "EditorCore/Compatibility.h"

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace editor::core {

using namespace detail;

namespace {

// Una differenza che impedisce di copiare un file così com'è nel formato del progetto.
struct Mismatch
{
    std::string message;   // es. "risoluzione 1280x720 invece di 1920x1080"
    // Solo i parametri di codifica di un video H.264 o HEVC (SPS/PPS, profilo): stessi codec, misure e fotogrammi al
    // secondo, quindi i pacchetti si possono copiare lo stesso, ripetendo i parametri giusti nel flusso (vedi
    // exportClips). Le altre differenze richiedono la ricodifica.
    bool parametersOnly = false;
};

std::string kindName(StreamKind kind)
{
    return kind == StreamKind::Video ? "video" : "audio";
}

std::string channelsText(const StreamSignature& s)
{
    return s.channelLayout.empty() ? std::to_string(s.channels)
                                   : std::to_string(s.channels) + " (" + s.channelLayout + ")";
}

int countOf(const MediaSignature& m, StreamKind kind)
{
    int count = 0;
    for (const StreamSignature& s : m.streams)
        count += s.kind == kind ? 1 : 0;
    return count;
}

bool parametersInStream(const StreamSignature& s)
{
    return s.kind == StreamKind::Video && (s.codec == "h264" || s.codec == "hevc");
}

// Differenze fra un flusso e quello corrispondente del formato di riferimento.
void compareStream(const StreamSignature& s, const StreamSignature& ref, std::vector<Mismatch>& out)
{
    const auto add = [&](const std::string& what) { out.push_back({what}); };
    // Una differenza dei soli parametri di codifica: per H.264 e HEVC i pacchetti si copiano lo stesso.
    const auto addParameters = [&](const std::string& what) { out.push_back({what, parametersInStream(s)}); };
    const std::string kind = kindName(s.kind);
    const std::size_t before = out.size();

    if (s.codec != ref.codec) {
        add("codec " + kind + " " + s.codec + " invece di " + ref.codec);
        return;  // con un codec diverso il resto non conta
    }
    if (!s.profile.empty() && !ref.profile.empty() && s.profile != ref.profile)
        addParameters("profilo " + kind + " " + s.profile + " invece di " + ref.profile);

    if (s.kind == StreamKind::Video) {
        if (s.width != ref.width || s.height != ref.height)
            add("risoluzione " + std::to_string(s.width) + "x" + std::to_string(s.height) + " invece di "
                + std::to_string(ref.width) + "x" + std::to_string(ref.height));
        if (s.pixelFormat != ref.pixelFormat)
            add("formato pixel " + s.pixelFormat + " invece di " + ref.pixelFormat);
        if (s.frameRate > 0 && ref.frameRate > 0 && std::abs(s.frameRate - ref.frameRate) > ref.frameRate * 0.001)
            add("frequenza fotogrammi " + formatFrameRate(s.frameRate) + " invece di " + formatFrameRate(ref.frameRate));
    } else {
        if (s.sampleRate != ref.sampleRate)
            add("frequenza di campionamento " + std::to_string(s.sampleRate) + " Hz invece di "
                + std::to_string(ref.sampleRate) + " Hz");
        if (s.channels != ref.channels || s.channelLayout != ref.channelLayout)
            add("canali " + channelsText(s) + " invece di " + channelsText(ref));
    }

    // Stessi codec e misure ma parametri di codifica diversi: il decoder del file unito userebbe quelli del primo
    // file anche per gli altri. Lo si segnala solo se tutto il resto coincide (altrimenti è una conseguenza delle
    // differenze già dette), a parte il profilo.
    const bool onlyProfile = std::all_of(out.begin() + static_cast<std::ptrdiff_t>(before), out.end(),
                                         [](const Mismatch& m) { return m.parametersOnly; });
    if (!onlyProfile || s.extradata == ref.extradata)
        return;
    if (s.codec == "h264") {
        // Per H.264 si guarda cosa cambia davvero: livello e informazioni sui tempi non contano, perché i
        // fotogrammi si decodificano allo stesso modo con i parametri del primo file. Formato e profondità del
        // colore, interlacciamento e proporzioni dei pixel cambiano l'immagine: quelli no.
        for (const H264Difference& difference : findH264Differences(parseH264Config(s.extradata), parseH264Config(ref.extradata))) {
            if (difference.changesPicture)
                add(difference.message);
            else
                addParameters(difference.message);
        }
        return;
    }
    if (s.kind == StreamKind::Video)
        addParameters("impostazioni dell'encoder video diverse");
    else
        add("impostazioni dell'encoder audio diverse");
}

// Differenze di un file rispetto a un altro (il riferimento): flussi presenti e, flusso per flusso, compareStream.
void compareFile(const MediaSignature& clip, const MediaSignature& ref, std::vector<Mismatch>& out)
{
    const int refVideo = countOf(ref, StreamKind::Video);
    const int refAudio = countOf(ref, StreamKind::Audio);
    const int video = countOf(clip, StreamKind::Video);
    const int audio = countOf(clip, StreamKind::Audio);
    bool sameLayout = video == refVideo && audio == refAudio;
    for (std::size_t s = 0; sameLayout && s < clip.streams.size(); ++s)
        sameLayout = clip.streams[s].kind == ref.streams[s].kind;
    if (!sameLayout) {
        out.push_back({std::to_string(video) + " flussi video e " + std::to_string(audio) + " audio invece di "
                       + std::to_string(refVideo) + " video e " + std::to_string(refAudio) + " audio"});
        return;
    }
    for (std::size_t s = 0; s < clip.streams.size(); ++s)
        compareStream(clip.streams[s], ref.streams[s], out);
}

}  // namespace

MediaSignature readMediaSignature(const std::string& utf8Path)
{
    FormatContextPtr fmt = openInput(utf8Path);
    MediaSignature signature;
    signature.path = utf8Path;
    signature.fileName = fileNameOf(utf8Path);

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* par = st->codecpar;
        const bool video = par->codec_type == AVMEDIA_TYPE_VIDEO && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
        const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !audio)
            continue;

        StreamSignature s;
        s.kind = video ? StreamKind::Video : StreamKind::Audio;
        readStreamFormat(fmt.get(), st, s);
        s.bFrames = video && par->video_delay > 0;
        if (par->extradata != nullptr && par->extradata_size > 0)
            s.extradata.assign(par->extradata, par->extradata + par->extradata_size);
        signature.streams.push_back(std::move(s));
    }
    return signature;
}

const StreamSignature* findStream(const std::vector<StreamSignature>& streams, StreamKind kind)
{
    const auto it = std::find_if(streams.begin(), streams.end(), [kind](const StreamSignature& s) { return s.kind == kind; });
    return it == streams.end() ? nullptr : &*it;
}

bool sameAudioFormat(const StreamSignature& a, const StreamSignature& b)
{
    std::vector<Mismatch> differences;
    compareStream(a, b, differences);
    return differences.empty();
}

const StreamSignature* ProjectFormat::video() const
{
    return findStream(streams, StreamKind::Video);
}

const StreamSignature* ProjectFormat::audio() const
{
    return findStream(streams, StreamKind::Audio);
}

ProjectFormat formatOf(const MediaSignature& signature)
{
    return {signature.fileName, signature.streams};
}

ClipFitResult classifyClip(const MediaSignature& clip, const ProjectFormat& format, bool withAudio)
{
    MediaSignature ref{"", format.sourceName, format.streams};
    MediaSignature other = clip;
    if (!withAudio) {
        std::erase_if(ref.streams, [](const StreamSignature& s) { return s.kind == StreamKind::Audio; });
        std::erase_if(other.streams, [](const StreamSignature& s) { return s.kind == StreamKind::Audio; });
    }
    std::vector<Mismatch> mismatches;
    compareFile(other, ref, mismatches);
    ClipFitResult result;
    for (const Mismatch& m : mismatches)
        result.reasons.push_back(m.message);
    if (mismatches.empty())
        result.fit = ClipFit::Copy;
    else if (std::all_of(mismatches.begin(), mismatches.end(), [](const Mismatch& m) { return m.parametersOnly; }))
        result.fit = ClipFit::CopyWithParameters;
    else
        result.fit = ClipFit::Reencode;
    return result;
}

}  // namespace editor::core
