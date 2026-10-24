#include "EditorCore/Project.h"

#include "EditorCore/AudioMixer.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>

namespace editor::core {

const std::vector<double>& Clip::keyframes() const
{
    static const std::vector<double> none;
    return keyframeData ? *keyframeData : none;
}

double Clip::snapToKeyframe(double seconds) const
{
    return core::snapToKeyframe(keyframes(), seconds);
}

int Project::add(std::string path)
{
    Clip clip;
    clip.id = nextId_++;
    clip.path = std::move(path);
    clips_.push_back(std::move(clip));
    return clips_.back().id;
}

void Project::setAnalysis(int id, std::vector<double> keyframes, double duration)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    Clip& clip = clips_[index];
    std::sort(keyframes.begin(), keyframes.end());
    clip.keyframeData = std::make_shared<const std::vector<double>>(std::move(keyframes));
    clip.duration = std::max(0.0, duration);
    clip.analyzed = true;
}

void Project::setClipPath(int id, std::string path)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    Clip& clip = clips_[index];
    clip.path = std::move(path);
    clip.analyzed = false;
    clip.keyframeData.reset();
    clip.duration = 0.0;
}

void Project::remove(int id)
{
    std::erase_if(clips_, [id](const Clip& c) { return c.id == id; });
    sequence_.removeClip(id);
}

void Project::clear()
{
    clips_.clear();
    sequence_.clear();
    audio_.clear();
    format_ = {};
}

const Clip* Project::find(int id) const
{
    const int index = indexOf(id);
    return index < 0 ? nullptr : &clips_[index];
}

int Project::indexOf(int id) const
{
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

bool Project::allAnalyzed() const
{
    return std::all_of(clips_.begin(), clips_.end(), [](const Clip& c) { return c.analyzed; });
}

std::vector<ClipRanges> Project::exportPlan(GapExport gaps) const
{
    std::vector<ClipRanges> plan;
    int previousClip = 0;
    for (const Fragment& f : sequence_.fragments()) {
        const Clip* clip = find(f.clipId);
        if (clip == nullptr)
            continue;
        if (plan.empty() || f.clipId != previousClip)
            plan.push_back({clip->path, {}, clip->keyframes()});
        plan.back().ranges.push_back({f.start, f.end});
        plan.back().gapsBefore.push_back(gaps == GapExport::Black ? f.gap : 0.0);
        previousClip = f.clipId;
    }
    return plan;
}

void Project::setAudioReplacement(std::optional<AudioReplacement> replacement)
{
    if (!replacement) {
        audio_.clear();
        return;
    }
    // Offset negativo: se ne salta l'inizio, cioè la clip parte da 0 nella sequenza e da -offset nel file.
    AudioTrack track;
    AudioClip clip;
    clip.path = std::move(replacement->path);
    clip.position = std::max(0.0, replacement->offset);
    clip.start = std::max(0.0, -replacement->offset);
    clip.sourceDuration = std::max(0.0, replacement->duration);
    clip.end = std::max(clip.start, clip.sourceDuration);
    track.clips.push_back(std::move(clip));
    AudioMix original;
    original.muted = true;
    audio_.clear();
    audio_.restore(original, {std::move(track)});
}

std::optional<AudioReplacement> Project::audioReplacement() const
{
    const AudioMix silenced{0.0, true, false};
    const auto& tracks = audio_.tracks();
    if (!(audio_.mix(AudioTracks::kOriginalTrack) == silenced) || tracks.size() != 1 || !(tracks[0].mix == AudioMix{})
        || tracks[0].clips.size() != 1)
        return std::nullopt;
    const AudioClip& clip = tracks[0].clips[0];
    if (clip.end != std::max(clip.start, clip.sourceDuration) || (clip.position > 0.0 && clip.start > 0.0)
        || !clip.envelope.empty())
        return std::nullopt;
    return AudioReplacement{clip.path, clip.position - clip.start, clip.sourceDuration};
}

void Project::setAudioOffset(double offset)
{
    if (auto replacement = audioReplacement()) {
        replacement->offset = offset;
        setAudioReplacement(std::move(replacement));
    }
}

void Project::restoreEdits(const Project& snapshot)
{
    std::vector<Clip> clips = snapshot.clips_;
    for (Clip& clip : clips) {
        const Clip* now = find(clip.id);
        if (now != nullptr && now->analyzed && now->path == clip.path) {
            clip.keyframeData = now->keyframeData;
            clip.duration = now->duration;
            clip.analyzed = true;
        }
    }
    clips_ = std::move(clips);
    sequence_ = snapshot.sequence_;
    audio_.restoreEdits(snapshot.audio_);
    format_ = snapshot.format_;
    nextId_ = std::max(nextId_, snapshot.nextId_);  // un file aggiunto dopo non riprende l'id di uno tolto
}

bool Project::moveFragment(int fragmentId, double start, bool* audioMoved, bool* audioStayed)
{
    if (audioMoved != nullptr)
        *audioMoved = false;
    if (audioStayed != nullptr)
        *audioStayed = false;
    // Dove sono i pezzi prima dello spostamento.
    struct Place
    {
        int id;
        double start;
        double length;
    };
    std::vector<Place> before;
    double t = 0.0;
    for (const Fragment& f : sequence_.fragments()) {
        t += f.gap;
        before.push_back({f.id, t, f.length()});
        t += f.length();
    }
    std::vector<std::pair<int, double>> shifts;
    if (!sequence_.moveTo(fragmentId, start, &shifts))
        return false;
    if (shifts.empty())
        return true;
    // Le clip delle tracce con la calamita accesa che toccano un pezzo spostato (nella sua posizione di prima) vanno con
    // lui, dello stesso tratto: tutte di seguito, quelle in testa nel verso dello spostamento per prime.
    struct Moved
    {
        int clip;
        int track;
        double position;
        double delta;
    };
    std::vector<Moved> moved;
    for (const AudioTrack& track : audio_.tracks()) {
        if (!track.mix.magnet)
            continue;
        for (const AudioClip& c : track.clips) {
            for (const auto& [id, delta] : shifts) {
                const auto place = std::find_if(before.begin(), before.end(), [&](const Place& p) { return p.id == id; });
                if (place != before.end() && c.endPosition() > place->start + kTimeEpsilon
                    && c.position < place->start + place->length - kTimeEpsilon) {
                    moved.push_back({c.id, track.id, c.position + delta, delta});
                    break;
                }
            }
        }
    }
    if (moved.empty())
        return true;
    const bool forward = std::any_of(shifts.begin(), shifts.end(), [](const auto& s) { return s.second > 0.0; });
    std::sort(moved.begin(), moved.end(), [&](const Moved& a, const Moved& b) {
        return forward ? a.position > b.position : a.position < b.position;
    });
    AudioTracks scratch = audio_;
    bool all = true;
    for (const Moved& m : moved)
        all = all && m.position >= 0.0 && scratch.moveTo(m.clip, m.track, m.position);
    if (all) {
        audio_ = std::move(scratch);
        if (audioMoved != nullptr)
            *audioMoved = true;
    } else if (audioStayed != nullptr) {
        *audioStayed = true;
    }
    return true;
}

bool Project::audioNeedsMix() const
{
    return audio_.needsMix() || std::any_of(sequence_.fragments().begin(), sequence_.fragments().end(),
                                            [](const Fragment& f) { return !f.envelope.empty(); });
}

AudioEdit Project::audioEdit(GapExport gaps) const
{
    AudioEdit edit;
    edit.replacement = audioReplacement();
    if (gaps == GapExport::Black) {
        edit.muted = sequence_.mutedRanges();  // i tempi della sequenza, spazi compresi
    } else {
        // GapExport::Close: i tempi della sequenza senza gli spazi vuoti, come nel file esportato.
        Sequence closed = sequence_;
        closed.closeAllGaps();
        edit.muted = closed.mutedRanges();
    }
    if (!edit.replacement && !sequence_.empty() && audioNeedsMix()) {
        edit.mix = std::make_shared<const MixSpec>(mixSpecOf(*this, false));
        edit.mixClosed = std::make_shared<const MixSpec>(mixSpecOf(*this, true));
    }
    return edit;
}

}  // namespace editor::core
