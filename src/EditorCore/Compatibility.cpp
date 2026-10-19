#include "EditorCore/Compatibility.h"

#include "EditorCore/FfmpegUtil.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include <cmath>
#include <cstdio>

namespace editor::core {

using namespace detail;

namespace {

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

// Differenze fra un flusso e quello corrispondente del primo file.
void compareStream(const StreamSignature& s, const StreamSignature& ref, const std::string& prefix,
                   int clipIndex, std::vector<Mismatch>& out)
{
    const auto add = [&](const std::string& what) { out.push_back({clipIndex, prefix + what}); };
    const std::string kind = kindName(s.kind);
    const std::size_t before = out.size();

    if (s.codec != ref.codec) {
        add("codec " + kind + " " + s.codec + " invece di " + ref.codec);
        return;  // con un codec diverso il resto non conta
    }
    if (!s.profile.empty() && !ref.profile.empty() && s.profile != ref.profile)
        add("profilo " + kind + " " + s.profile + " invece di " + ref.profile);

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

    // Stessi codec e misure ma parametri di codifica diversi (es. SPS/PPS di H.264): il decoder del file unito
    // userebbe quelli del primo file anche per gli altri, con immagini rovinate.
    // Lo si segnala solo se tutto il resto coincide: altrimenti è una conseguenza delle differenze già dette.
    if (out.size() == before && s.extradata != ref.extradata)
        add("parametri di codifica " + kind + " diversi"
            + (s.kind == StreamKind::Video ? std::string(" (SPS/PPS)") : std::string()));
}

}  // namespace

MediaSignature readMediaSignature(const std::string& utf8Path)
{
    FormatContextPtr fmt = openInput(utf8Path);
    MediaSignature signature;
    signature.path = utf8Path;
    const auto name = pathFromUtf8(utf8Path).filename().u8string();
    signature.fileName.assign(name.begin(), name.end());

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* par = st->codecpar;
        const bool video = par->codec_type == AVMEDIA_TYPE_VIDEO && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
        const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !audio)
            continue;

        StreamSignature s;
        s.kind = video ? StreamKind::Video : StreamKind::Audio;
        s.codec = avcodec_get_name(par->codec_id);
        if (const char* profile = avcodec_profile_name(par->codec_id, par->profile))
            s.profile = profile;
        if (par->extradata != nullptr && par->extradata_size > 0)
            s.extradata.assign(par->extradata, par->extradata + par->extradata_size);
        if (video) {
            s.width = par->width;
            s.height = par->height;
            if (const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format)))
                s.pixelFormat = pix;
            const AVRational rate = av_guess_frame_rate(fmt.get(), const_cast<AVStream*>(st), nullptr);
            s.frameRate = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
        } else {
            s.sampleRate = par->sample_rate;
            s.channels = par->ch_layout.nb_channels;
            AVChannelLayout layout = {};
            if (par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&layout, par->ch_layout.nb_channels);
            else
                av_channel_layout_copy(&layout, &par->ch_layout);
            char text[128] = {};
            if (av_channel_layout_describe(&layout, text, sizeof(text)) > 0)
                s.channelLayout = text;
            av_channel_layout_uninit(&layout);
        }
        signature.streams.push_back(std::move(s));
    }
    return signature;
}

std::vector<Mismatch> checkCompatibility(const std::vector<MediaSignature>& clips)
{
    std::vector<Mismatch> result;
    if (clips.size() < 2)
        return result;
    const MediaSignature& ref = clips.front();
    const int refVideo = countOf(ref, StreamKind::Video);
    const int refAudio = countOf(ref, StreamKind::Audio);

    for (std::size_t c = 1; c < clips.size(); ++c) {
        const MediaSignature& clip = clips[c];
        const int index = static_cast<int>(c);
        const std::string prefix = "Clip " + std::to_string(c + 1) + " (" + clip.fileName + "): ";

        const int video = countOf(clip, StreamKind::Video);
        const int audio = countOf(clip, StreamKind::Audio);
        bool sameLayout = video == refVideo && audio == refAudio;
        for (std::size_t s = 0; sameLayout && s < clip.streams.size(); ++s)
            sameLayout = clip.streams[s].kind == ref.streams[s].kind;
        if (!sameLayout) {
            result.push_back({index, prefix + std::to_string(video) + " flussi video e " + std::to_string(audio)
                                         + " audio invece di " + std::to_string(refVideo) + " video e "
                                         + std::to_string(refAudio) + " audio"});
            continue;
        }
        for (std::size_t s = 0; s < clip.streams.size(); ++s)
            compareStream(clip.streams[s], ref.streams[s], prefix, index, result);
    }
    return result;
}

std::string describeMismatches(const std::vector<Mismatch>& mismatches)
{
    std::string text;
    for (const Mismatch& m : mismatches) {
        if (!text.empty())
            text += '\n';
        text += m.message;
    }
    return text;
}

}  // namespace editor::core
