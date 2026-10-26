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
    text_.clear();
    groups_.clear();
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
        || tracks[0].clips.size() != 1 || tracks[0].host)
        return std::nullopt;
    const AudioClip& clip = tracks[0].clips[0];
    if (clip.end != std::max(clip.start, clip.sourceDuration) || (clip.position > 0.0 && clip.start > 0.0)
        || !clip.envelope.empty())
        return std::nullopt;
    return AudioReplacement{clip.path, clip.position - clip.start, clip.sourceDuration};
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
    text_.restoreEdits(snapshot.text_);
    groups_.restoreEdits(snapshot.groups_);
    format_ = snapshot.format_;
    nextId_ = std::max(nextId_, snapshot.nextId_);  // un file aggiunto dopo non riprende l'id di uno tolto
}

bool Project::objectExists(ObjectRef object) const
{
    switch (object.kind) {
    case ObjectRef::Kind::Piece:
        return sequence_.find(object.id) != nullptr;
    case ObjectRef::Kind::PieceAudio: {
        const Fragment* f = sequence_.find(object.id);
        return f != nullptr && !f->detached;
    }
    case ObjectRef::Kind::AudioClip:
        return audio_.find(object.id) != nullptr;
    case ObjectRef::Kind::TextClip:
        return text_.find(object.id) != nullptr;
    }
    return false;
}

std::vector<ObjectRef> Project::groupMembers(ObjectRef object) const
{
    std::vector<ObjectRef> found;
    if (!objectExists(object))
        return found;
    found.push_back(object);
    for (std::size_t i = 0; i < found.size(); ++i) {
        const ObjectRef current = found[i];
        const auto add = [&](ObjectRef other) {
            if (objectExists(other) && std::find(found.begin(), found.end(), other) == found.end())
                found.push_back(other);
        };
        // Il pezzo video e il suo audio.
        if (current.kind == ObjectRef::Kind::Piece)
            add({ObjectRef::Kind::PieceAudio, current.id});
        else if (current.kind == ObjectRef::Kind::PieceAudio)
            add({ObjectRef::Kind::Piece, current.id});
        // Il gruppo dell'utente.
        if (const ObjectGroup* group = groups_.find(groups_.groupOf(current))) {
            for (const ObjectRef& member : group->members)
                add(member);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

int Project::groupObjects(const std::vector<ObjectRef>& objects)
{
    std::vector<ObjectRef> valid;
    for (const ObjectRef& o : objects) {
        if (objectExists(o) && std::find(valid.begin(), valid.end(), o) == valid.end())
            valid.push_back(o);
    }
    if (valid.size() < 2)
        return 0;
    const std::vector<ObjectRef> together = groupMembers(valid.front());
    if (std::all_of(valid.begin(), valid.end(), [&](const ObjectRef& o) { return std::find(together.begin(), together.end(), o) != together.end(); }))
        return 0;  // sono già legati
    return groups_.create(valid);
}

std::vector<ObjectRef> Project::splitGroup(ObjectRef anchor, double seconds)
{
    std::vector<ObjectRef> created;
    const std::vector<ObjectRef> members = groupMembers(anchor);
    const auto isMember = [&](ObjectRef::Kind kind, int id) {
        return std::find(members.begin(), members.end(), ObjectRef{kind, id}) != members.end();
    };
    const int index = sequence_.indexAt(seconds);
    if (index >= 0 && isMember(ObjectRef::Kind::Piece, sequence_.fragments()[static_cast<std::size_t>(index)].id)) {
        if (const int id = sequence_.split(seconds); id != 0)
            created.push_back({ObjectRef::Kind::Piece, id});
    }
    for (const ObjectRef& m : members) {
        if (m.kind != ObjectRef::Kind::AudioClip)
            continue;
        const AudioClip* clip = audio_.find(m.id);
        if (clip == nullptr || seconds <= clip->position + kTimeEpsilon || seconds >= clip->position + clip->length() - kTimeEpsilon)
            continue;
        if (const int id = audio_.split(audio_.trackOf(m.id), seconds); id != 0)
            created.push_back({ObjectRef::Kind::AudioClip, id});
    }
    for (const ObjectRef& m : members) {
        if (m.kind != ObjectRef::Kind::TextClip)
            continue;
        if (const int id = text_.split(m.id, seconds); id != 0)
            created.push_back({ObjectRef::Kind::TextClip, id});
    }
    if (created.size() >= 2)
        groups_.create(created);
    return created;
}

int Project::removeObjects(const std::vector<ObjectRef>& objects)
{
    int removed = 0;
    for (const ObjectRef& o : objects) {
        if (o.kind == ObjectRef::Kind::AudioClip && audio_.remove(o.id))
            ++removed;
        else if (o.kind == ObjectRef::Kind::TextClip && text_.remove(o.id))
            ++removed;
    }
    for (const ObjectRef& o : objects) {
        if (o.kind != ObjectRef::Kind::PieceAudio)
            continue;
        const bool pieceGoes = std::find(objects.begin(), objects.end(), ObjectRef{ObjectRef::Kind::Piece, o.id}) != objects.end();
        const int index = sequence_.indexOf(o.id);
        if (pieceGoes || index < 0)
            continue;
        const double start = sequence_.startOf(index);
        if (sequence_.muteRange(start, start + sequence_.fragments()[static_cast<std::size_t>(index)].length()))
            ++removed;
    }
    for (const ObjectRef& o : objects) {
        if (o.kind == ObjectRef::Kind::Piece && sequence_.remove(o.id))
            ++removed;
    }
    return removed;
}

Project::UngroupResult Project::ungroupObjects(const std::vector<ObjectRef>& objects)
{
    UngroupResult result;
    for (const ObjectRef& o : objects) {
        if (const int id = groups_.groupOf(o); id != 0 && groups_.dissolve(id))
            ++result.dissolved;
    }
    if (result.dissolved > 0)
        return result;
    std::vector<int> pieces;
    for (const ObjectRef& o : objects) {
        if ((o.kind == ObjectRef::Kind::Piece || o.kind == ObjectRef::Kind::PieceAudio)
            && std::find(pieces.begin(), pieces.end(), o.id) == pieces.end())
            pieces.push_back(o.id);
    }
    for (int fragmentId : pieces) {
        if (unlinkAudio(fragmentId) != 0)
            ++result.audioUnlinked;
    }
    return result;
}

std::vector<std::vector<ObjectRef>> Project::validGroups() const
{
    std::vector<std::vector<ObjectRef>> result;
    for (const ObjectGroup& g : groups_.all()) {
        std::vector<ObjectRef> members;
        for (const ObjectRef& o : g.members) {
            if (objectExists(o))
                members.push_back(o);
        }
        if (members.size() >= 2)
            result.push_back(std::move(members));
    }
    return result;
}

int Project::unlinkAudio(int fragmentId, int* track)
{
    const Fragment* f = sequence_.find(fragmentId);
    const Clip* source = f != nullptr ? find(f->clipId) : nullptr;
    if (source == nullptr)
        return 0;
    // Già tutto silenziato: niente da scollegare.
    if (f->muted.size() == 1 && f->muted.front().start <= f->start + 1e-9 && f->muted.front().end >= f->end - 1e-9)
        return 0;
    const double from = sequence_.startOf(sequence_.indexOf(fragmentId));
    const double length = f->length();
    // Resta sulla corsia A1, staccato dai pezzi video: lì deve esserci posto.
    const int target = AudioTracks::kOriginalTrack;
    if (std::abs(audio_.freePosition(target, length, from) - from) > 1e-9)
        return 0;
    AudioTracks scratch = audio_;
    AudioClip clip;
    clip.path = source->path;
    clip.start = f->start;
    clip.end = f->end;
    clip.position = from;
    clip.sourceDuration = source->duration;
    clip.envelope = f->envelope;
    const std::vector<TimeRange> muted = f->muted;
    const int id = scratch.add(target, clip);
    if (id == 0)
        return 0;
    for (const TimeRange& r : muted)
        scratch.removeRange(target, from + (r.start - f->start), from + (r.end - f->start));
    audio_ = std::move(scratch);
    sequence_.muteRange(from, from + length);
    sequence_.setDetached(fragmentId, true);
    sequence_.setEnvelope(fragmentId, {});
    if (track != nullptr)
        *track = target;
    return id;
}

bool Project::relinkAudio(int fragmentId)
{
    const Fragment* f = sequence_.find(fragmentId);
    const Clip* source = f != nullptr ? find(f->clipId) : nullptr;
    if (source == nullptr)
        return false;
    const double from = sequence_.startOf(sequence_.indexOf(fragmentId));
    const double to = from + f->length();
    const double offset = from - f->start;  // sequenza - file, per l'audio del pezzo
    constexpr double kAligned = 1e-6;
    std::vector<int> tracks;
    std::vector<LevelPoint> line;
    int found = 0;
    for (const AudioTrack& t : audio_.tracks()) {
        for (const AudioClip& c : t.clips) {
            if (c.path == source->path && std::abs((c.position - c.start) - offset) < kAligned && c.position < to - 1e-9
                && c.endPosition() > from + 1e-9) {
                tracks.push_back(t.id);
                line = c.envelope;
                ++found;
            }
        }
    }
    if (found == 0)
        return false;
    for (int trackId : tracks)
        audio_.removeRange(trackId, from, to);
    sequence_.unmuteRange(from, to);
    sequence_.setDetached(fragmentId, false);
    if (found == 1)
        sequence_.setEnvelope(fragmentId, std::move(line));
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
