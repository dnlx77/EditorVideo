#include "EditorCore/Project.h"
#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace editor::core {

namespace {

constexpr int kVideoLane = -1;  // i pezzi video; le clip audio hanno l'id della loro traccia

// Un pezzo video o una clip audio, con il gruppo a cui appartiene (le componenti di Project::groupMembers).
struct Item
{
    ObjectRef ref;
    int lane = kVideoLane;
    int track = 0;      // per le clip
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
    // Le componenti: oggetti legati fra loro (gruppi dell'utente, pezzo e suo audio).
    int next = 0;
    for (Item& item : scene.items)
        item.component = -1;
    for (Item& item : scene.items) {
        if (item.component >= 0)
            continue;
        for (const ObjectRef& member : project.groupMembers(item.ref)) {
            for (Item& other : scene.items) {
                if (other.ref == member)
                    other.component = next;
            }
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

}  // namespace

int Project::groupMoverCount(ObjectRef anchor) const
{
    const Scene scene = sceneOf(*this, anchor);
    if (scene.anchorComponent < 0)
        return 0;
    return static_cast<int>(std::count_if(scene.items.begin(), scene.items.end(),
                                          [&](const Item& i) { return i.component == scene.anchorComponent; }));
}

double Project::clampGroupDelta(ObjectRef anchor, double delta, bool push) const
{
    const Scene scene = sceneOf(*this, anchor);
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

bool Project::moveGroup(ObjectRef anchor, double delta, bool push)
{
    Scene scene = sceneOf(*this, anchor);
    if (scene.anchorComponent < 0 || !std::isfinite(delta))
        return false;
    if (std::abs(delta) <= kTimeEpsilon)
        return true;
    if (std::abs(clampGroupDelta(anchor, delta, push) - delta) > kTimeEpsilon)
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
    for (const Item& item : scene.items) {
        const double d = shift.count(item.component) ? shift[item.component] : 0.0;
        if (std::abs(d) <= kTimeEpsilon)
            continue;
        if (item.ref.kind == ObjectRef::Kind::Piece)
            pieces.emplace_back(item.ref.id, d);
        else if (item.ref.kind == ObjectRef::Kind::AudioClip)
            clips.push_back({item.ref.id, item.track, item.start + d});
    }
    Sequence sequence = sequence_;
    if (!pieces.empty() && !sequence.shiftPieces(pieces))
        return false;
    AudioTracks audio = audio_;
    std::stable_sort(clips.begin(), clips.end(), [&](const ClipMove& a, const ClipMove& b) {
        return delta > 0.0 ? a.position > b.position : a.position < b.position;
    });
    for (const ClipMove& m : clips) {
        if (!audio.moveTo(m.clip, m.track, m.position))
            return false;
    }
    sequence_ = std::move(sequence);
    audio_ = std::move(audio);
    return true;
}

}  // namespace editor::core
