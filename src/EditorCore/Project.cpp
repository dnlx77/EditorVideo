#include "EditorCore/Project.h"

#include "EditorCore/AudioMixer.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
    pruneTransitions();
}

void Project::clear()
{
    clips_.clear();
    sequence_.clear();
    audio_.clear();
    text_.clear();
    transitions_.clear();
    textStyles_.clear();
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
        plan.back().transforms.push_back(f.transform);
        plan.back().animations.push_back(f.animation);
        previousClip = f.clipId;
    }
    return plan;
}

Project Project::withEmptySpaceClosed() const
{
    using Range = std::pair<double, double>;
    const std::vector<double> starts = sequence_.starts();
    std::vector<Range> gapRanges;
    for (std::size_t i = 0; i < sequence_.fragments().size(); ++i) {
        const double gap = sequence_.fragments()[i].gap;
        if (gap > 1e-9)
            gapRanges.emplace_back(starts[i] - gap, starts[i]);
    }
    if (gapRanges.empty())
        return *this;
    // Tutto quello che occupa tempo oltre ai pezzi di V1.
    std::vector<Range> busy;
    for (const TextLayer* layer : [&] {
             std::vector<const TextLayer*> all{&text_.baseLayer()};
             for (const TextLayer& l : text_.layers())
                 all.push_back(&l);
             return all;
         }()) {
        for (const TextClip& c : layer->clips)
            busy.emplace_back(c.position, c.endPosition());
        for (const LayerPiece& p : layer->pieces)
            busy.emplace_back(p.position, p.endPosition());
    }
    for (const AudioTrack& track : audio_.tracks()) {
        for (const AudioClip& c : track.clips)
            busy.emplace_back(c.position, c.endPosition());
    }
    std::sort(busy.begin(), busy.end());
    // I tratti vuoti: dentro gli spazi di V1, fuori da tutto il resto.
    std::vector<Range> empty;
    for (const Range& gap : gapRanges) {
        double at = gap.first;
        for (const Range& b : busy) {
            if (b.second <= at || b.first >= gap.second)
                continue;
            if (b.first > at + 1e-9)
                empty.emplace_back(at, b.first);
            at = std::max(at, b.second);
            if (at >= gap.second)
                break;
        }
        if (gap.second > at + 1e-9)
            empty.emplace_back(at, gap.second);
    }
    if (empty.empty())
        return *this;
    // Quanto si è tolto prima del punto `t` (nessun tratto vuoto lo attraversa: chi inizia in `t` non è dentro uno).
    const auto removedBefore = [&](double t) {
        double removed = 0.0;
        for (const Range& e : empty) {
            if (e.second <= t + 1e-9)
                removed += e.second - e.first;
        }
        return removed;
    };
    Project result = *this;
    std::vector<std::pair<int, double>> deltas;
    for (std::size_t i = 0; i < sequence_.fragments().size(); ++i) {
        if (const double r = removedBefore(starts[i]); r > 1e-9)
            deltas.emplace_back(sequence_.fragments()[i].id, -r);
    }
    if (!result.sequence_.shiftPieces(deltas))
        return *this;
    // Scritte, pezzi dei livelli e clip audio: dal primo all'ultimo, ognuno sulla sua corsia.
    struct Move
    {
        double from;
        int kind;  // 0 scritta, 1 pezzo di livello, 2 clip audio
        int id;
        int lane;
    };
    std::vector<Move> moves;
    {
        std::vector<std::pair<int, const TextLayer*>> all{{TextTracks::kBaseLayerId, &text_.baseLayer()}};
        for (const TextLayer& l : text_.layers())
            all.emplace_back(l.id, &l);
        for (const auto& [id, layer] : all) {
            for (const TextClip& c : layer->clips)
                moves.push_back({c.position, 0, c.id, id});
            for (const LayerPiece& p : layer->pieces)
                moves.push_back({p.position, 1, p.id, id});
        }
        for (const AudioTrack& track : audio_.tracks()) {
            for (const AudioClip& c : track.clips)
                moves.push_back({c.position, 2, c.id, track.id});
        }
    }
    std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.from < b.from; });
    for (const Move& m : moves) {
        const double to = std::max(0.0, m.from - removedBefore(m.from));
        if (std::abs(to - m.from) < 1e-9)
            continue;
        bool ok = false;
        if (m.kind == 0)
            ok = result.textTracks().moveTo(m.id, m.lane, to);
        else if (m.kind == 1)
            ok = result.textTracks().movePieceTo(m.id, m.lane, to);
        else
            ok = result.audioTracks().moveTo(m.id, m.lane, to);
        if (!ok)
            return *this;
    }
    return result;
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
    transitions_.restoreEdits(snapshot.transitions_);
    textStyles_ = snapshot.textStyles_;
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
    case ObjectRef::Kind::LayerPiece:
        return text_.findPiece(object.id) != nullptr;
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

int Project::groupObjects(const std::vector<ObjectRef>& objects, int* relinked)
{
    std::vector<ObjectRef> valid;
    for (const ObjectRef& o : objects) {
        if (objectExists(o) && std::find(valid.begin(), valid.end(), o) == valid.end())
            valid.push_back(o);
    }
    // Un pezzo con l'audio scollegato e la sua clip audio (ancora allineata) tornano un oggetto solo, come prima di U.
    for (const ObjectRef& piece : std::vector<ObjectRef>(valid)) {
        const Fragment* f = piece.kind == ObjectRef::Kind::Piece ? sequence_.find(piece.id) : nullptr;
        const Clip* source = f != nullptr && f->detached ? find(f->clipId) : nullptr;
        if (source == nullptr)
            continue;
        const double from = sequence_.startOf(sequence_.indexOf(piece.id));
        const double offset = from - f->start;
        std::vector<ObjectRef> aligned;
        for (const ObjectRef& o : valid) {
            const AudioClip* c = o.kind == ObjectRef::Kind::AudioClip ? audio_.find(o.id) : nullptr;
            if (c != nullptr && c->path == source->path && std::abs((c->position - c->start) - offset) < 1e-6
                && c->position < from + f->length() - 1e-9 && c->endPosition() > from + 1e-9)
                aligned.push_back(o);
        }
        if (aligned.empty() || !relinkAudio(piece.id))
            continue;
        if (relinked != nullptr)
            ++*relinked;
        std::erase_if(valid, [&](const ObjectRef& o) { return !objectExists(o); });
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
        const int first = sequence_.fragments()[static_cast<std::size_t>(index)].id;
        if (const int id = sequence_.split(seconds); id != 0) {
            created.push_back({ObjectRef::Kind::Piece, id});
            transitions_.moveJunction(first, id);  // la transizione sulla giunzione dopo il pezzo passa alla sua seconda parte
        }
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
    for (const ObjectRef& m : members) {
        if (m.kind != ObjectRef::Kind::LayerPiece)
            continue;
        if (const int id = text_.splitPiece(m.id, seconds); id != 0)
            created.push_back({ObjectRef::Kind::LayerPiece, id});
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
        else if (o.kind == ObjectRef::Kind::LayerPiece && text_.removePiece(o.id))
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

bool Project::addToGroup(int groupId, ObjectRef object)
{
    return objectExists(object) && groups_.addTo(groupId, object);
}

bool Project::removeFromGroup(ObjectRef object)
{
    return groups_.remove(object);
}

bool Project::renameGroup(int groupId, const std::string& name)
{
    return groups_.rename(groupId, name);
}

bool Project::dropOnGroup(const std::vector<ObjectRef>& objects, int groupId)
{
    std::vector<ObjectRef> valid;
    for (const ObjectRef& o : objects) {
        if (objectExists(o) && std::find(valid.begin(), valid.end(), o) == valid.end())
            valid.push_back(o);
    }
    if (valid.empty())
        return false;
    if (groupId > 0) {
        if (groups_.find(groupId) == nullptr)
            return false;
        bool changed = false;
        for (const ObjectRef& o : valid)
            changed = groups_.addTo(groupId, o) || changed;
        return changed;
    }
    if (valid.size() == 1)
        return groups_.remove(valid.front());
    return groups_.create(valid) != 0;
}

std::vector<ObjectGroup> Project::validGroupList() const
{
    std::vector<ObjectGroup> result;
    for (const ObjectGroup& g : groups_.all()) {
        ObjectGroup valid;
        valid.id = g.id;
        valid.name = g.name;
        for (const ObjectRef& o : g.members) {
            if (objectExists(o))
                valid.members.push_back(o);
        }
        if (valid.members.size() >= 2)
            result.push_back(std::move(valid));
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

int Project::audioTrackForLayer(int layerId)
{
    const int index = text_.layerIndex(layerId);
    if (index < 0)
        return -1;
    std::vector<int> added;
    for (const AudioTrack& t : audio_.tracks()) {
        if (!t.host)
            added.push_back(t.id);
    }
    while (static_cast<int>(added.size()) <= index)
        added.push_back(audio_.addTrack());
    return added[static_cast<std::size_t>(index)];
}

int Project::placeAudioClip(AudioClip clip, int layerId)
{
    const double length = clip.length();
    // La traccia che corrisponde al livello (V2 con A2, V3 con A3...) se lì c'è posto; altrimenti A1 se libera e fuori
    // dall'audio dei pezzi di V1; poi le altre tracce aggiunte; poi una traccia nuova.
    int track = -1;
    if (layerId > 0) {
        const int own = audioTrackForLayer(layerId);
        if (own >= 0 && std::abs(audio_.freePosition(own, length, clip.position) - clip.position) <= kTimeEpsilon)
            track = own;
    }
    const std::vector<TimeRange> blocked = linkedAudioRanges();
    const auto overlapsLinked = [&](double from) {
        return std::any_of(blocked.begin(), blocked.end(), [&](const TimeRange& r) { return from < r.end - kTimeEpsilon && from + length > r.start + kTimeEpsilon; });
    };
    if (track < 0 && std::abs(audio_.freePosition(AudioTracks::kOriginalTrack, length, clip.position) - clip.position) <= kTimeEpsilon
        && !overlapsLinked(clip.position))
        track = AudioTracks::kOriginalTrack;
    for (std::size_t i = 0; track < 0 && i < audio_.tracks().size(); ++i) {
        const AudioTrack& t = audio_.tracks()[i];
        if (!t.host && std::abs(audio_.freePosition(t.id, length, clip.position) - clip.position) <= kTimeEpsilon)
            track = t.id;
    }
    if (track < 0)
        track = audio_.addTrack();
    return audio_.add(track, std::move(clip));
}

int Project::addLayerPiece(int layerId, const LayerPiece& piece, bool withAudio, int* audioClip)
{
    if (audioClip != nullptr)
        *audioClip = 0;
    const Clip* source = find(piece.clipId);
    const int id = text_.addPiece(layerId, piece);
    if (id == 0 || !withAudio || source == nullptr)
        return id;
    AudioClip clip;
    clip.path = source->path;
    clip.start = piece.start;
    clip.end = piece.end;
    clip.position = piece.position;
    clip.sourceDuration = source->duration;
    const int clipId = placeAudioClip(clip, layerId);
    if (clipId == 0)
        return id;
    if (audioClip != nullptr)
        *audioClip = clipId;
    groups_.create({{ObjectRef::Kind::LayerPiece, id}, {ObjectRef::Kind::AudioClip, clipId}});
    return id;
}

int Project::liftToLayer(int fragmentId, int layerId, double position, int* audioClip)
{
    if (audioClip != nullptr)
        *audioClip = 0;
    const Fragment* f = sequence_.find(fragmentId);
    const Clip* source = f != nullptr ? find(f->clipId) : nullptr;
    if (source == nullptr || text_.layer(layerId) == nullptr)
        return 0;
    const double length = f->length();
    const double where = text_.freeStartFor(layerId, length, position);
    if (where < 0.0)
        return 0;
    const Fragment fragment = *f;
    const Project backup = *this;
    // Il pezzo lascia uno spazio vuoto in V1; il suo audio (se era collegato) diventa una clip libera accanto al nuovo pezzo.
    sequence_.remove(fragmentId, true);
    LayerPiece piece;
    piece.clipId = fragment.clipId;
    piece.start = fragment.start;
    piece.end = fragment.end;
    piece.position = where;
    piece.transform = fragment.transform;
    piece.animation = fragment.animation;
    const int id = text_.addPiece(layerId, piece);
    if (id == 0) {
        *this = backup;
        return 0;
    }
    int clipId = 0;
    if (!fragment.detached) {
        AudioClip clip;
        clip.path = source->path;
        clip.start = fragment.start;
        clip.end = fragment.end;
        clip.position = where;
        clip.sourceDuration = source->duration;
        clip.envelope = fragment.envelope;
        clipId = placeAudioClip(clip, layerId);
        if (clipId != 0) {
            // I tratti senza audio restano senza audio.
            const int track = audio_.trackOf(clipId);
            for (const TimeRange& r : fragment.muted)
                audio_.removeRange(track, where + (r.start - fragment.start), where + (r.end - fragment.start));
        }
    }
    if (clipId != 0) {
        groups_.create({{ObjectRef::Kind::LayerPiece, id}, {ObjectRef::Kind::AudioClip, clipId}});
        if (audioClip != nullptr)
            *audioClip = clipId;
    }
    pruneTransitions();
    return id;
}

int Project::lowerToV1(int pieceId, double position, bool push, int* audioClip)
{
    if (audioClip != nullptr)
        *audioClip = 0;
    const LayerPiece* p = text_.findPiece(pieceId);
    if (p == nullptr)
        return 0;
    const LayerPiece piece = *p;
    // L'audio in gruppo con il pezzo (se c'è) resta una clip a parte: il pezzo in V1 ha l'audio scollegato.
    int clipId = 0;
    for (const ObjectRef& m : groupMembers({ObjectRef::Kind::LayerPiece, pieceId})) {
        if (m.kind == ObjectRef::Kind::AudioClip && audio_.find(m.id) != nullptr)
            clipId = m.id;
    }
    const int id = sequence_.insertAt(piece.clipId, piece.start, piece.end, position, push);
    if (id == 0)
        return 0;
    text_.removePiece(pieceId);
    sequence_.setTransform(id, piece.transform);
    sequence_.setAnimation(id, piece.animation);
    if (clipId == 0) {
        // Il pezzo non ha un audio suo (era scollegato, o è stato messo senza audio): in V1 resta muto, senza un audio
        // collegato nuovo che si sommerebbe alla clip libera che magari c'è ancora.
        const double from = sequence_.startOf(sequence_.indexOf(id));
        sequence_.muteRange(from, from + piece.length());
        sequence_.setDetached(id, true);
    }
    if (clipId != 0) {
        const AudioClip* clip = audio_.find(clipId);
        const Clip* source = find(piece.clipId);
        const bool sameAudio = clip != nullptr && source != nullptr && clip->path == source->path
                               && std::abs(clip->start - piece.start) < 1e-6 && std::abs(clip->end - piece.end) < 1e-6;
        if (sameAudio) {
            // L'audio è quello del video com'era: torna collegato su A1 (con la sua linea del livello) e la clip sparisce.
            sequence_.setEnvelope(id, clip->envelope);
            audio_.remove(clipId);
        } else {
            const double from = sequence_.startOf(sequence_.indexOf(id));
            sequence_.muteRange(from, from + piece.length());
            sequence_.setDetached(id, true);
            groups_.create({{ObjectRef::Kind::Piece, id}, {ObjectRef::Kind::AudioClip, clipId}});
            if (audioClip != nullptr)
                *audioClip = clipId;
        }
    }
    pruneTransitions();
    return id;
}

int Project::setTransform(const std::vector<int>& fragmentIds, const Transform& transform)
{
    int changed = 0;
    for (int id : fragmentIds)
        changed += sequence_.setTransform(id, transform) ? 1 : 0;
    return changed;
}

int Project::setAnimation(const std::vector<int>& fragmentIds, const TransformAnimation& animation)
{
    int changed = 0;
    for (int id : fragmentIds)
        changed += sequence_.setAnimation(id, animation) ? 1 : 0;
    return changed;
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
    if (audio_.needsMix() || std::any_of(sequence_.fragments().begin(), sequence_.fragments().end(),
                                         [](const Fragment& f) { return !f.envelope.empty(); }))
        return true;
    // La dissolvenza incrociata di una transizione: l'audio dei due pezzi non si può più copiare com'è.
    const std::vector<TransitionPlacement> placements = activeTransitions();
    return std::any_of(placements.begin(), placements.end(), [this](const TransitionPlacement& t) {
        const TransitionClip* clip = transitions_.find(t.id);
        return clip != nullptr && clip->crossfadeAudio;
    });
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

// --- Transizioni

namespace {

// Come si divide una durata `duration` fra prima e dopo la giunzione: a metà, se i margini lo permettono; se da un lato
// manca spazio, tutto il resto dall'altro. Presuppone duration <= limits.max().
std::pair<double, double> splitDuration(double duration, const TransitionLimits& limits)
{
    double before = std::min(duration / 2.0, limits.before);
    double after = duration - before;
    if (after > limits.after) {
        after = limits.after;
        before = duration - after;
    }
    return {before, after};
}

}  // namespace

bool Project::isJunction(int beforeFragment) const
{
    const int index = sequence_.indexOf(beforeFragment);
    const auto& fragments = sequence_.fragments();
    if (index < 0 || static_cast<std::size_t>(index) + 1 >= fragments.size())
        return false;
    const Clip* clip = find(fragments[static_cast<std::size_t>(index)].clipId);
    return fragments[static_cast<std::size_t>(index) + 1].gap <= kTimeEpsilon && clip != nullptr && clip->duration > 0.0;
}

TransitionLimits Project::transitionLimits(int beforeFragment) const
{
    if (!isJunction(beforeFragment))
        return {};
    const auto& fragments = sequence_.fragments();
    const std::size_t index = static_cast<std::size_t>(sequence_.indexOf(beforeFragment));
    const Fragment& a = fragments[index];
    const Fragment& b = fragments[index + 1];
    const double tail = std::max(0.0, find(a.clipId)->duration - a.end);
    TransitionLimits limits;
    limits.before = std::min(b.start, a.length());
    limits.after = std::min(tail, b.length());
    // Le transizioni si contano da sinistra: quella sulla giunzione prima di `a` ha già preso la sua parte di `a`.
    if (index > 0) {
        if (const TransitionClip* previous = transitions_.atJunction(fragments[index - 1].id)) {
            if (const auto placed = transitionPlacement(previous->id))
                limits.before = std::max(0.0, std::min(limits.before, a.length() - placed->after));
        }
    }
    return limits;
}

int Project::addTransition(int beforeFragment, const std::string& type, double duration)
{
    if (findTransition(type) == nullptr || transitions_.atJunction(beforeFragment) != nullptr)
        return 0;
    const TransitionLimits limits = transitionLimits(beforeFragment);
    if (duration < kMinTransition - kTimeEpsilon || duration > limits.max() + kTimeEpsilon)
        return 0;
    TransitionClip clip;
    clip.type = type;
    clip.beforeFragment = beforeFragment;
    clip.afterFragment = sequence_.fragments()[static_cast<std::size_t>(sequence_.indexOf(beforeFragment)) + 1].id;
    clip.duration = std::min(duration, limits.max());
    return transitions_.add(std::move(clip));
}

bool Project::removeTransition(int id)
{
    return transitions_.remove(id);
}

bool Project::setTransitionDuration(int id, double duration)
{
    TransitionClip* clip = transitions_.find(id);
    if (clip == nullptr)
        return false;
    const TransitionLimits limits = transitionLimits(clip->beforeFragment);
    if (duration < kMinTransition - kTimeEpsilon || duration > limits.max() + kTimeEpsilon)
        return false;
    clip->duration = std::min(duration, limits.max());
    return true;
}

bool Project::setTransitionSettings(int id, const std::string& type, const TransitionParams& params, bool crossfadeAudio)
{
    TransitionClip* clip = transitions_.find(id);
    if (clip == nullptr || findTransition(type) == nullptr)
        return false;
    clip->type = type;
    clip->params = params;
    clip->crossfadeAudio = crossfadeAudio;
    return true;
}

std::optional<TransitionPlacement> Project::transitionPlacement(int id) const
{
    const TransitionClip* clip = transitions_.find(id);
    if (clip == nullptr || findTransition(clip->type) == nullptr)
        return std::nullopt;
    const int index = sequence_.indexOf(clip->beforeFragment);
    const auto& fragments = sequence_.fragments();
    if (index < 0 || static_cast<std::size_t>(index) + 1 >= fragments.size()
        || fragments[static_cast<std::size_t>(index) + 1].id != clip->afterFragment)
        return std::nullopt;
    const TransitionLimits limits = transitionLimits(clip->beforeFragment);
    if (limits.max() <= 0.0 || clip->duration < kMinTransition - kTimeEpsilon || clip->duration > limits.max() + kTimeEpsilon)
        return std::nullopt;
    const auto [before, after] = splitDuration(std::min(clip->duration, limits.max()), limits);
    TransitionPlacement placement;
    placement.id = id;
    placement.beforeFragment = clip->beforeFragment;
    placement.afterFragment = clip->afterFragment;
    placement.junction = sequence_.startOf(index + 1);
    placement.before = before;
    placement.after = after;
    return placement;
}

std::vector<TransitionPlacement> Project::activeTransitions() const
{
    std::vector<TransitionPlacement> placements;
    for (const TransitionClip& clip : transitions_.clips()) {
        if (const auto placed = transitionPlacement(clip.id))
            placements.push_back(*placed);
    }
    std::sort(placements.begin(), placements.end(),
              [](const TransitionPlacement& a, const TransitionPlacement& b) { return a.junction < b.junction; });
    return placements;
}

int Project::pruneTransitions()
{
    // Da sinistra a destra: i limiti di una transizione dipendono da quella prima.
    std::vector<int> ids;
    for (const TransitionClip& clip : transitions_.clips())
        ids.push_back(clip.id);
    std::sort(ids.begin(), ids.end(), [this](int a, int b) {
        return sequence_.indexOf(transitions_.find(a)->beforeFragment) < sequence_.indexOf(transitions_.find(b)->beforeFragment);
    });
    int touched = 0;
    for (const int id : ids) {
        TransitionClip* clip = transitions_.find(id);
        const int index = sequence_.indexOf(clip->beforeFragment);
        const auto& fragments = sequence_.fragments();
        const bool attached = index >= 0 && static_cast<std::size_t>(index) + 1 < fragments.size()
                              && fragments[static_cast<std::size_t>(index) + 1].id == clip->afterFragment;
        // Finché il file del pezzo non è analizzato (progetto appena aperto) non si sa quanto margine c'è: si lascia com'è.
        if (attached) {
            const Clip* source = find(fragments[static_cast<std::size_t>(index)].clipId);
            if (source != nullptr && source->duration <= 0.0)
                continue;
        }
        const TransitionLimits limits = attached ? transitionLimits(clip->beforeFragment) : TransitionLimits{};
        if (!attached || limits.max() < kMinTransition - kTimeEpsilon) {
            transitions_.remove(id);
            ++touched;
        } else if (clip->duration > limits.max() + kTimeEpsilon) {
            clip->duration = limits.max();
            ++touched;
        }
    }
    return touched;
}

}  // namespace editor::core

namespace editor::core {

namespace {

constexpr double kJunctionGap = 1e-4;     // quanto possono distare due bordi per dirli attaccati (secondi)
constexpr double kJunctionSameDb = 1e-3;  // e due livelli per dirli uguali

}  // namespace

std::vector<Project::Junction> Project::junctions() const
{
    std::vector<Junction> result;
    const std::vector<Fragment>& fragments = sequence_.fragments();
    const std::vector<double> starts = sequence_.starts();
    for (std::size_t i = 0; i + 1 < fragments.size(); ++i) {
        if (fragments[i].detached || fragments[i + 1].detached)
            continue;
        if (std::abs(starts[i] + fragments[i].length() - starts[i + 1]) <= kJunctionGap)
            result.push_back({true, fragments[i].id, fragments[i + 1].id});
    }
    for (const AudioTrack& track : audio_.tracks()) {
        for (std::size_t i = 0; i + 1 < track.clips.size(); ++i) {
            if (std::abs(track.clips[i].endPosition() - track.clips[i + 1].position) <= kJunctionGap)
                result.push_back({false, track.clips[i].id, track.clips[i + 1].id});
        }
    }
    return result;
}

std::optional<double> Project::junctionLevel(const Junction& junction) const
{
    const std::vector<LevelPoint>* a = nullptr;
    const std::vector<LevelPoint>* b = nullptr;
    double aEdge = 0.0;
    double bEdge = 0.0;
    if (junction.fragment) {
        const Fragment* first = sequence_.find(junction.first);
        const Fragment* second = sequence_.find(junction.second);
        if (first == nullptr || second == nullptr)
            return std::nullopt;
        a = &first->envelope;
        b = &second->envelope;
        aEdge = first->end;
        bEdge = second->start;
    } else {
        const AudioClip* first = audio_.find(junction.first);
        const AudioClip* second = audio_.find(junction.second);
        if (first == nullptr || second == nullptr)
            return std::nullopt;
        a = &first->envelope;
        b = &second->envelope;
        aEdge = first->end;
        bEdge = second->start;
    }
    const LevelPoint* pa = pointAt(*a, aEdge);
    const LevelPoint* pb = pointAt(*b, bEdge);
    if (pa == nullptr || pb == nullptr || std::abs(pa->db - pb->db) > kJunctionSameDb)
        return std::nullopt;
    return pa->db;
}

bool Project::setJunctionLevel(const Junction& junction, double db)
{
    db = std::clamp(db, kLevelMinDb, kLevelMaxDb);
    if (junction.fragment) {
        const Fragment* first = sequence_.find(junction.first);
        const Fragment* second = sequence_.find(junction.second);
        if (first == nullptr || second == nullptr)
            return false;
        const std::vector<LevelPoint> a = withPointAt(first->envelope, first->end, db);
        const std::vector<LevelPoint> b = withPointAt(second->envelope, second->start, db);
        const bool changedA = sequence_.setEnvelope(junction.first, a);
        const bool changedB = sequence_.setEnvelope(junction.second, b);
        return changedA || changedB;
    }
    const AudioClip* first = audio_.find(junction.first);
    const AudioClip* second = audio_.find(junction.second);
    if (first == nullptr || second == nullptr)
        return false;
    const std::vector<LevelPoint> a = withPointAt(first->envelope, first->end, db);
    const std::vector<LevelPoint> b = withPointAt(second->envelope, second->start, db);
    const bool changedA = audio_.setClipEnvelope(junction.first, a);
    const bool changedB = audio_.setClipEnvelope(junction.second, b);
    return changedA || changedB;
}

bool Project::removeJunctionPoint(const Junction& junction)
{
    if (junction.fragment) {
        const Fragment* first = sequence_.find(junction.first);
        const Fragment* second = sequence_.find(junction.second);
        if (first == nullptr || second == nullptr)
            return false;
        const std::vector<LevelPoint> a = withoutPointAt(first->envelope, first->end);
        const std::vector<LevelPoint> b = withoutPointAt(second->envelope, second->start);
        const bool changedA = sequence_.setEnvelope(junction.first, a);
        const bool changedB = sequence_.setEnvelope(junction.second, b);
        return changedA || changedB;
    }
    const AudioClip* first = audio_.find(junction.first);
    const AudioClip* second = audio_.find(junction.second);
    if (first == nullptr || second == nullptr)
        return false;
    const std::vector<LevelPoint> a = withoutPointAt(first->envelope, first->end);
    const std::vector<LevelPoint> b = withoutPointAt(second->envelope, second->start);
    const bool changedA = audio_.setClipEnvelope(junction.first, a);
    const bool changedB = audio_.setClipEnvelope(junction.second, b);
    return changedA || changedB;
}

int Project::linkNewJunctions(const std::vector<Junction>& before)
{
    int created = 0;
    for (const Junction& junction : junctions()) {
        if (junction.fragment || std::find(before.begin(), before.end(), junction) != before.end())
            continue;
        const AudioClip* first = audio_.find(junction.first);
        const AudioClip* second = audio_.find(junction.second);
        if (first == nullptr || second == nullptr || pointAt(first->envelope, first->end) != nullptr
            || pointAt(second->envelope, second->start) != nullptr)
            continue;
        const double a = envelopeDbAt(first->envelope, first->end);
        if (std::abs(a - envelopeDbAt(second->envelope, second->start)) > kJunctionSameDb)
            continue;
        if (setJunctionLevel(junction, a))
            ++created;
    }
    return created;
}

}  // namespace editor::core
