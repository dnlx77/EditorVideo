#include "EditorCore/Project.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace editor::core {

namespace {

constexpr int kVideoLane = -1;  // i pezzi video; le clip audio hanno l'id della loro traccia
constexpr int kTextLaneBase = -1000;  // le scritte: kTextLaneBase - id del livello (mai uguale a una traccia audio)

// Un pezzo video o una clip audio, con il gruppo a cui appartiene (le componenti di Project::groupMembers).
struct Item
{
    ObjectRef ref;
    int lane = kVideoLane;
    int track = 0;      // per le clip (la traccia audio o il livello di scritte)
    double start = 0.0;
    double length = 0.0;
    int component = 0;

    double end() const { return start + length; }
};

struct Scene
{
    std::vector<Item> items;
    int anchorComponent = -1;
};

Scene sceneOf(const Project& project, ObjectRef anchor)
{
    Scene scene;
    const Sequence& sequence = project.sequence();
    const std::vector<double> starts = sequence.starts();
    for (std::size_t i = 0; i < sequence.fragments().size(); ++i) {
        Item item;
        item.ref = {ObjectRef::Kind::Piece, sequence.fragments()[i].id};
        item.start = starts[i];
        item.length = sequence.fragments()[i].length();
        scene.items.push_back(item);
    }
    // L'audio dei pezzi (non scollegato) sta su A1: le clip libere di A1 non ci si sovrappongono.
    for (const Fragment& f : sequence.fragments()) {
        if (f.detached)
            continue;
        const std::size_t i = static_cast<std::size_t>(&f - sequence.fragments().data());
        Item item;
        item.ref = {ObjectRef::Kind::PieceAudio, f.id};
        item.lane = AudioTracks::kOriginalTrack;
        item.track = AudioTracks::kOriginalTrack;
        item.start = starts[i];
        item.length = f.length();
        scene.items.push_back(item);
    }
    for (const AudioTrack& track : project.audioTracks().tracks()) {
        for (const AudioClip& clip : track.clips) {
            Item item;
            item.ref = {ObjectRef::Kind::AudioClip, clip.id};
            item.lane = track.id;
            item.track = track.id;
            item.start = clip.position;
            item.length = clip.length();
            scene.items.push_back(item);
        }
    }
    for (const TextLayer& layer : project.textTracks().layers()) {
        for (const TextClip& clip : layer.clips) {
            Item item;
            item.ref = {ObjectRef::Kind::TextClip, clip.id};
            item.lane = kTextLaneBase - layer.id;
            item.track = layer.id;
            item.start = clip.position;
            item.length = clip.length;
            scene.items.push_back(item);
        }
    }
    // Le componenti: oggetti legati fra loro (gruppi dell'utente, pezzo e suo audio).
    std::map<ObjectRef, std::size_t> indexOf;
    for (std::size_t i = 0; i < scene.items.size(); ++i) {
        scene.items[i].component = -1;
        indexOf[scene.items[i].ref] = i;
    }
    int next = 0;
    for (Item& item : scene.items) {
        if (item.component >= 0)
            continue;
        for (const ObjectRef& member : project.groupMembers(item.ref)) {
            if (const auto found = indexOf.find(member); found != indexOf.end())
                scene.items[found->second].component = next;
        }
        if (item.component < 0)
            item.component = next;
        ++next;
    }
    // Anche il pezzo e il suo audio: chi parte da PieceAudio vale come il pezzo.
    const ObjectRef start = anchor.kind == ObjectRef::Kind::PieceAudio ? ObjectRef{ObjectRef::Kind::Piece, anchor.id} : anchor;
    for (const Item& item : scene.items) {
        if (item.ref == start)
            scene.anchorComponent = item.component;
    }
    return scene;
}

int moverCount(const Scene& scene)
{
    if (scene.anchorComponent < 0)
        return 0;
    return static_cast<int>(std::count_if(scene.items.begin(), scene.items.end(),
                                          [&](const Item& i) {
                                              return i.component == scene.anchorComponent && i.ref.kind != ObjectRef::Kind::PieceAudio;
                                          }));
}

double clampDelta(const Scene& scene, double delta, bool push)
{
    if (scene.anchorComponent < 0)
        return 0.0;
    double lowest = -std::numeric_limits<double>::infinity();
    double highest = std::numeric_limits<double>::infinity();
    for (const Item& mover : scene.items) {
        if (mover.component != scene.anchorComponent)
            continue;
        lowest = std::max(lowest, -mover.start);  // non prima di 0
        for (const Item& other : scene.items) {
            if (other.component == scene.anchorComponent || other.lane != mover.lane)
                continue;
            if (other.start >= mover.end() - kTimeEpsilon)
                highest = std::min(highest, other.start - mover.end());
            else if (other.end() <= mover.start + kTimeEpsilon)
                lowest = std::max(lowest, other.end() - mover.start);
        }
    }
    if (push)
        highest = std::numeric_limits<double>::infinity();  // verso destra si spinge
    return std::clamp(delta, std::min(lowest, 0.0), std::max(highest, 0.0));
}

// Sposta di `delta` il gruppo della scena (e, con la spinta, quelli che urta); false e nessuna modifica se non si può.
bool applyMove(Project& project, const Scene& scene, double delta, bool push)
{
    if (scene.anchorComponent < 0 || !std::isfinite(delta))
        return false;
    if (std::abs(delta) <= kTimeEpsilon)
        return true;
    if (std::abs(clampDelta(scene, delta, push) - delta) > kTimeEpsilon)
        return false;
    // Di quanto si sposta ogni gruppo: quello preso `delta`, gli altri solo se spinti (verso destra).
    std::map<int, double> shift;
    shift[scene.anchorComponent] = delta;
    if (push && delta > 0.0) {
        bool changed = true;
        for (int round = 0; changed && round < 100000; ++round) {
            changed = false;
            std::map<int, std::vector<const Item*>> lanes;
            for (const Item& item : scene.items)
                lanes[item.lane].push_back(&item);
            for (auto& [lane, items] : lanes) {
                std::stable_sort(items.begin(), items.end(), [](const Item* a, const Item* b) { return a->start < b->start; });
                for (std::size_t i = 0; i + 1 < items.size(); ++i) {
                    const Item& left = *items[i];
                    const Item& right = *items[i + 1];
                    if (left.component == right.component)
                        continue;
                    const double leftEnd = left.end() + shift[left.component];
                    const double need = leftEnd - right.start;
                    if (need > shift[right.component] + kTimeEpsilon) {
                        shift[right.component] = need;
                        changed = true;
                    }
                }
            }
        }
        if (changed)
            return false;  // non si assesta (non dovrebbe succedere)
    }
    // Applica: i pezzi in una volta, le clip una alla volta dalla più avanti (verso destra) o dalla più indietro.
    std::vector<std::pair<int, double>> pieces;
    struct ClipMove
    {
        int clip;
        int track;
        double position;
    };
    std::vector<ClipMove> clips;
    std::vector<ClipMove> texts;
    for (const Item& item : scene.items) {
        const double d = shift.count(item.component) ? shift[item.component] : 0.0;
        if (std::abs(d) <= kTimeEpsilon)
            continue;
        if (item.ref.kind == ObjectRef::Kind::Piece)
            pieces.emplace_back(item.ref.id, d);
        else if (item.ref.kind == ObjectRef::Kind::AudioClip)
            clips.push_back({item.ref.id, item.track, item.start + d});
        else if (item.ref.kind == ObjectRef::Kind::TextClip)
            texts.push_back({item.ref.id, item.track, item.start + d});
    }
    Sequence sequence = project.sequence();
    if (!pieces.empty() && !sequence.shiftPieces(pieces))
        return false;
    AudioTracks audio = project.audioTracks();
    std::stable_sort(clips.begin(), clips.end(), [&](const ClipMove& a, const ClipMove& b) {
        return delta > 0.0 ? a.position > b.position : a.position < b.position;
    });
    for (const ClipMove& m : clips) {
        if (!audio.moveTo(m.clip, m.track, m.position))
            return false;
    }
    TextTracks text = project.textTracks();
    std::stable_sort(texts.begin(), texts.end(), [&](const ClipMove& a, const ClipMove& b) {
        return delta > 0.0 ? a.position > b.position : a.position < b.position;
    });
    for (const ClipMove& m : texts) {
        if (!text.moveTo(m.clip, m.track, m.position))
            return false;
    }
    project.sequence() = std::move(sequence);
    project.audioTracks() = std::move(audio);
    project.textTracks() = std::move(text);
    return true;
}

}  // namespace

std::vector<TimeRange> Project::linkedAudioRanges() const
{
    std::vector<TimeRange> ranges;
    const std::vector<double> starts = sequence_.starts();
    for (std::size_t i = 0; i < sequence_.fragments().size(); ++i) {
        const Fragment& f = sequence_.fragments()[i];
        if (!f.detached)
            ranges.push_back({starts[i], starts[i] + f.length()});
    }
    return ranges;
}

int Project::groupMoverCount(ObjectRef anchor) const
{
    return moverCount(sceneOf(*this, anchor));
}

double Project::clampGroupDelta(ObjectRef anchor, double delta, bool push) const
{
    return clampDelta(sceneOf(*this, anchor), delta, push);
}

bool Project::moveGroup(ObjectRef anchor, double delta, bool push)
{
    return applyMove(*this, sceneOf(*this, anchor), delta, push);
}

double Project::freeSpotFor(ObjectRef object, int track, double desired, bool push) const
{
    double now = 0.0;
    if (object.kind == ObjectRef::Kind::AudioClip) {
        const AudioClip* clip = audio_.find(object.id);
        if (clip == nullptr)
            return -1.0;
        now = clip->position;
    } else if (object.kind == ObjectRef::Kind::TextClip) {
        const TextClip* clip = text_.find(object.id);
        if (clip == nullptr)
            return -1.0;
        now = clip->position;
    } else {
        const int index = sequence_.indexOf(object.id);
        if (index < 0)
            return -1.0;
        now = sequence_.startOf(index);
    }
    const Scene scene = sceneOf(*this, object);
    if (moverCount(scene) > 1)
        return now + clampDelta(scene, desired - now, push);
    if (object.kind == ObjectRef::Kind::TextClip)
        return text_.nearestFreeStart(object.id, track, desired);
    return object.kind == ObjectRef::Kind::AudioClip
               ? audio_.nearestFreePosition(object.id, track, desired, track == AudioTracks::kOriginalTrack ? linkedAudioRanges() : std::vector<TimeRange>{})
               : sequence_.nearestFreeStart(object.id, desired, push);
}

Project::MoveResult Project::moveObject(ObjectRef object, int track, double start, bool push)
{
    MoveResult result;
    const bool isText = object.kind == ObjectRef::Kind::TextClip;
    const bool isClip = object.kind == ObjectRef::Kind::AudioClip;
    const AudioClip* clip = isClip ? audio_.find(object.id) : nullptr;
    const TextClip* textClip = isText ? text_.find(object.id) : nullptr;
    const int index = isClip || isText ? -1 : sequence_.indexOf(object.id);
    if ((isClip && clip == nullptr) || (isText && textClip == nullptr) || (!isClip && !isText && index < 0))
        return result;
    const Scene scene = sceneOf(*this, object);
    if (moverCount(scene) > 1) {
        // Nel gruppo si muove tutto insieme, ognuno sulla sua traccia.
        const double now = isClip ? clip->position : isText ? textClip->position : sequence_.startOf(index);
        const double delta = clampDelta(scene, start - now, push);
        result.group = true;
        result.moved = std::abs(delta) >= 0.0005 && applyMove(*this, scene, delta, push);
        return result;
    }
    if (isText)
        result.moved = text_.moveTo(object.id, track, start);
    else
        result.moved = isClip ? audio_.moveTo(object.id, track, start,
                                              track == AudioTracks::kOriginalTrack ? linkedAudioRanges() : std::vector<TimeRange>{})
                              : sequence_.moveTo(object.id, start, nullptr, push);
    return result;
}

}  // namespace editor::core
