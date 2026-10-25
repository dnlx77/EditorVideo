#include "EditorCore/AudioTracks.h"

#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace editor::core {

int AudioTracks::addTrack()
{
    AudioTrack track;
    track.id = nextTrackId_++;
    tracks_.push_back(std::move(track));
    return tracks_.back().id;
}

bool AudioTracks::removeTrack(int trackId)
{
    if (trackId == kOriginalTrack)
        return false;
    return std::erase_if(tracks_, [trackId](const AudioTrack& t) { return t.id == trackId; }) > 0;
}

const AudioTrack* AudioTracks::track(int trackId) const
{
    const int index = trackIndex(trackId);
    return index < 0 ? nullptr : &tracks_[static_cast<std::size_t>(index)];
}

int AudioTracks::trackIndex(int trackId) const
{
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == trackId)
            return static_cast<int>(i);
    }
    return -1;
}

std::string AudioTracks::trackName(int trackId) const
{
    if (trackId == kOriginalTrack)
        return "A1";
    const int index = trackIndex(trackId);
    return index < 0 ? std::string() : "A" + std::to_string(index + 2 - (hasHost() ? 1 : 0));
}

AudioTrack* AudioTracks::hostTrack()
{
    if (!hasHost()) {
        AudioTrack host;
        host.id = kOriginalTrack;
        host.host = true;
        tracks_.insert(tracks_.begin(), std::move(host));
    }
    return &tracks_.front();
}

void AudioTracks::pruneHost()
{
    if (hasHost() && tracks_.front().clips.empty())
        tracks_.erase(tracks_.begin());
}

AudioTrack* AudioTracks::mutableTrack(int trackId)
{
    const int index = trackIndex(trackId);
    return index < 0 ? nullptr : &tracks_[static_cast<std::size_t>(index)];
}

AudioMix* AudioTracks::mutableMix(int trackId)
{
    if (trackId == kOriginalTrack)
        return &original_;
    AudioTrack* t = mutableTrack(trackId);
    return t == nullptr ? nullptr : &t->mix;
}

const AudioMix& AudioTracks::mix(int trackId) const
{
    const AudioTrack* t = trackId == kOriginalTrack ? nullptr : track(trackId);
    return t == nullptr ? original_ : t->mix;
}

bool AudioTracks::setLevel(int trackId, double db)
{
    AudioMix* m = mutableMix(trackId);
    if (m == nullptr || !std::isfinite(db))
        return false;
    db = std::clamp(db, kMinLevelDb, kMaxLevelDb);
    if (m->levelDb == db)
        return false;
    m->levelDb = db;
    return true;
}

bool AudioTracks::setMuted(int trackId, bool muted)
{
    AudioMix* m = mutableMix(trackId);
    if (m == nullptr || m->muted == muted)
        return false;
    m->muted = muted;
    return true;
}

bool AudioTracks::setSolo(int trackId, bool solo)
{
    AudioMix* m = mutableMix(trackId);
    if (m == nullptr || m->solo == solo)
        return false;
    m->solo = solo;
    return true;
}

bool AudioTracks::audible(int trackId) const
{
    if (trackId != kOriginalTrack && track(trackId) == nullptr)
        return false;
    const bool anySolo = original_.solo || std::any_of(tracks_.begin(), tracks_.end(), [](const AudioTrack& t) {
                             return t.mix.solo;
                         });
    const AudioMix& m = mix(trackId);
    return !m.muted && (!anySolo || m.solo);
}

bool AudioTracks::setClipEnvelope(int clipId, std::vector<LevelPoint> points)
{
    int t = 0;
    int index = 0;
    if (!locate(clipId, t, index))
        return false;
    AudioClip& clip = tracks_[static_cast<std::size_t>(t)].clips[static_cast<std::size_t>(index)];
    points = normalizeEnvelope(std::move(points));
    if (clip.envelope == points)
        return false;
    clip.envelope = std::move(points);
    return true;
}

bool AudioTracks::needsMix() const
{
    if (isDefault())
        return false;
    if (!audible(kOriginalTrack) || original_.levelDb != 0.0)
        return true;
    return std::any_of(tracks_.begin(), tracks_.end(), [](const AudioTrack& t) { return !t.clips.empty(); });
}

double AudioTracks::gain(int trackId) const
{
    return audible(trackId) ? dbToGain(mix(trackId).levelDb) : 0.0;
}

double AudioTracks::dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

bool AudioTracks::overlaps(const AudioTrack& track, double position, double length, int ignoredClip)
{
    return std::any_of(track.clips.begin(), track.clips.end(), [&](const AudioClip& c) {
        return c.id != ignoredClip && position < c.endPosition() - kTimeEpsilon
               && position + length > c.position + kTimeEpsilon;
    });
}

void AudioTracks::sortClips(AudioTrack& track)
{
    std::stable_sort(track.clips.begin(), track.clips.end(),
                     [](const AudioClip& a, const AudioClip& b) { return a.position < b.position; });
}

bool AudioTracks::locate(int clipId, int& trackIndex, int& clipIndex) const
{
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        const auto& clips = tracks_[t].clips;
        for (std::size_t c = 0; c < clips.size(); ++c) {
            if (clips[c].id == clipId) {
                trackIndex = static_cast<int>(t);
                clipIndex = static_cast<int>(c);
                return true;
            }
        }
    }
    return false;
}

int AudioTracks::add(int trackId, AudioClip clip)
{
    if (clip.length() <= kTimeEpsilon || clip.start < 0.0 || clip.position < -kTimeEpsilon)
        return 0;
    AudioTrack* t = trackId == kOriginalTrack ? hostTrack() : mutableTrack(trackId);
    if (t == nullptr)
        return 0;
    clip.position = std::max(0.0, clip.position);
    if (overlaps(*t, clip.position, clip.length(), 0)) {
        pruneHost();
        return 0;
    }
    clip.id = nextClipId_++;
    t->clips.push_back(std::move(clip));
    sortClips(*t);
    return nextClipId_ - 1;
}

bool AudioTracks::remove(int clipId)
{
    int t = 0;
    int c = 0;
    if (!locate(clipId, t, c))
        return false;
    auto& clips = tracks_[static_cast<std::size_t>(t)].clips;
    clips.erase(clips.begin() + c);
    pruneHost();
    return true;
}

const AudioClip* AudioTracks::find(int clipId) const
{
    int t = 0;
    int c = 0;
    if (!locate(clipId, t, c))
        return nullptr;
    return &tracks_[static_cast<std::size_t>(t)].clips[static_cast<std::size_t>(c)];
}

int AudioTracks::trackOf(int clipId) const
{
    int t = 0;
    int c = 0;
    return locate(clipId, t, c) ? tracks_[static_cast<std::size_t>(t)].id : -1;
}

double AudioTracks::nearestFreePosition(int clipId, int trackId, double desired) const
{
    const AudioClip* clip = find(clipId);
    static const AudioTrack kNoClips;
    const AudioTrack* t = trackId == kOriginalTrack && !hasHost() ? &kNoClips : track(trackId);
    if (clip == nullptr || t == nullptr)
        return -1.0;
    return freeSpot(*t, clip->length(), desired, clipId);
}

double AudioTracks::freePosition(int trackId, double length, double desired) const
{
    static const AudioTrack kNoClips;
    const AudioTrack* t = trackId == kOriginalTrack && !hasHost() ? &kNoClips : track(trackId);
    return t == nullptr ? -1.0 : freeSpot(*t, length, desired, 0);
}

double AudioTracks::freeSpot(const AudioTrack& track, double length, double desired, int ignoredClip)
{
    desired = std::max(0.0, desired);
    if (!overlaps(track, desired, length, ignoredClip))
        return desired;
    // Contro una delle altre clip, prima o dopo: il punto libero più vicino.
    double best = -1.0;
    for (const AudioClip& other : track.clips) {
        if (other.id == ignoredClip)
            continue;
        for (const double candidate : {other.endPosition(), other.position - length}) {
            if (candidate < 0.0 || overlaps(track, candidate, length, ignoredClip))
                continue;
            if (best < 0.0 || std::abs(candidate - desired) < std::abs(best - desired))
                best = candidate;
        }
    }
    return best < 0.0 ? desired : best;
}

bool AudioTracks::moveTo(int clipId, int trackId, double position)
{
    int from = 0;
    int index = 0;
    if (!locate(clipId, from, index) || position < -kTimeEpsilon)
        return false;
    if (trackId != kOriginalTrack && mutableTrack(trackId) == nullptr)
        return false;
    position = std::max(0.0, position);
    AudioClip clip = tracks_[static_cast<std::size_t>(from)].clips[static_cast<std::size_t>(index)];
    // Le tracce non cambiano posto nell'elenco finché non si crea/toglie la traccia di A1: si lavora sugli id.
    const int fromId = tracks_[static_cast<std::size_t>(from)].id;
    AudioTrack* to = trackId == kOriginalTrack ? hostTrack() : mutableTrack(trackId);
    if (overlaps(*to, position, clip.length(), clipId)) {
        pruneHost();
        return false;
    }
    AudioTrack* source = mutableTrack(fromId);
    source->clips.erase(std::find_if(source->clips.begin(), source->clips.end(), [clipId](const AudioClip& c) { return c.id == clipId; }));
    clip.position = position;
    to->clips.push_back(std::move(clip));
    sortClips(*to);
    pruneHost();
    return true;
}

int AudioTracks::split(int trackId, double seconds)
{
    AudioTrack* t = mutableTrack(trackId);
    if (t == nullptr)
        return 0;
    for (std::size_t i = 0; i < t->clips.size(); ++i) {
        AudioClip& first = t->clips[i];
        if (seconds <= first.position + kTimeEpsilon || seconds >= first.endPosition() - kTimeEpsilon)
            continue;
        AudioClip second = first;
        second.id = nextClipId_++;
        second.start = first.start + (seconds - first.position);
        second.position = seconds;
        first.end = second.start;
        t->clips.insert(t->clips.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(second));
        return nextClipId_ - 1;
    }
    return 0;
}

double AudioTracks::trimLimit(int clipId, Sequence::Edge edge, double desired, double minLength, bool ripple) const
{
    int t = 0;
    int index = 0;
    if (!locate(clipId, t, index))
        return -1.0;
    const auto& clips = tracks_[static_cast<std::size_t>(t)].clips;
    const AudioClip& c = clips[static_cast<std::size_t>(index)];
    minLength = std::max(minLength, kTimeEpsilon);
    if (edge == Sequence::Edge::Start) {
        // Senza ripple l'inizio nella sequenza si sposta: non prima di 0 né della fine della clip precedente.
        double lowest = 0.0;
        if (!ripple) {
            const double previousEnd = index > 0 ? clips[static_cast<std::size_t>(index) - 1].endPosition() : 0.0;
            lowest = std::max(0.0, c.start - (c.position - previousEnd));
        }
        return std::clamp(desired, std::min(lowest, c.start), std::max(c.start, c.end - minLength));
    }
    // Senza ripple la fine nella sequenza si sposta: non oltre l'inizio della clip dopo.
    double highest = c.sourceDuration > 0 ? c.sourceDuration : std::numeric_limits<double>::max();
    if (!ripple && static_cast<std::size_t>(index) + 1 < clips.size())
        highest = std::min(highest, c.end + (clips[static_cast<std::size_t>(index) + 1].position - c.endPosition()));
    return std::clamp(desired, std::min(c.start + minLength, c.end), std::max(highest, c.end));
}

bool AudioTracks::trim(int clipId, Sequence::Edge edge, double value, double minLength, bool ripple)
{
    int t = 0;
    int index = 0;
    if (!locate(clipId, t, index))
        return false;
    value = trimLimit(clipId, edge, value, minLength, ripple);
    auto& clips = tracks_[static_cast<std::size_t>(t)].clips;
    AudioClip& c = clips[static_cast<std::size_t>(index)];
    const double old = edge == Sequence::Edge::Start ? c.start : c.end;
    if (std::abs(value - old) <= kTimeEpsilon)
        return false;
    const double delta = value - old;
    c.envelope = envelopeForTrim(c.envelope, edge == Sequence::Edge::Start, old, value, edge == Sequence::Edge::Start ? c.end : c.start);
    double shift = 0.0;  // di quanto scorrono le clip dopo (con ripple)
    if (edge == Sequence::Edge::Start) {
        c.start = value;
        if (ripple)
            shift = -delta;
        else
            c.position = std::max(0.0, c.position + delta);  // la fine resta ferma nella sequenza
    } else {
        c.end = value;
        if (ripple)
            shift = delta;
    }
    for (std::size_t i = static_cast<std::size_t>(index) + 1; i < clips.size(); ++i)
        clips[i].position = std::max(0.0, clips[i].position + shift);
    return true;
}

bool AudioTracks::removeRange(int trackId, double from, double to)
{
    AudioTrack* t = mutableTrack(trackId);
    if (t == nullptr || to <= from + kTimeEpsilon)
        return false;
    bool changed = false;
    std::vector<AudioClip> kept;
    for (const AudioClip& c : t->clips) {
        if (c.endPosition() <= from + kTimeEpsilon || c.position >= to - kTimeEpsilon) {
            kept.push_back(c);
            continue;
        }
        changed = true;
        bool keptId = false;
        if (c.position < from - kTimeEpsilon) {  // resta la parte prima
            AudioClip before = c;
            before.end = c.start + (from - c.position);
            kept.push_back(before);
            keptId = true;
        }
        if (c.endPosition() > to + kTimeEpsilon) {  // resta la parte dopo
            AudioClip after = c;
            if (keptId)
                after.id = nextClipId_++;
            after.start = c.start + (to - c.position);
            after.position = to;
            kept.push_back(after);
        }
    }
    t->clips = std::move(kept);
    pruneHost();
    return changed;
}

int AudioTracks::relocate(const std::string& path, const std::string& newPath)
{
    int count = 0;
    for (AudioTrack& t : tracks_) {
        for (AudioClip& c : t.clips) {
            if (c.path == path) {
                c.path = newPath;
                ++count;
            }
        }
    }
    return count;
}

int AudioTracks::removePath(const std::string& path)
{
    int count = 0;
    for (AudioTrack& t : tracks_)
        count += static_cast<int>(std::erase_if(t.clips, [&](const AudioClip& c) { return c.path == path; }));
    pruneHost();
    return count;
}

void AudioTracks::restore(AudioMix original, std::vector<AudioTrack> tracks)
{
    original_ = original;
    tracks_ = std::move(tracks);
    // Le clip libere di A1 (una sola traccia, in testa).
    std::stable_partition(tracks_.begin(), tracks_.end(), [](const AudioTrack& t) { return t.host; });
    for (std::size_t i = 1; i < tracks_.size() && tracks_.front().host; ++i) {
        if (tracks_[i].host) {
            tracks_.front().clips.insert(tracks_.front().clips.end(), tracks_[i].clips.begin(), tracks_[i].clips.end());
            tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(i));
            --i;
        }
    }
    for (AudioTrack& t : tracks_) {
        t.id = t.host ? kOriginalTrack : nextTrackId_++;
        t.mix = t.host ? AudioMix{} : t.mix;
        for (AudioClip& c : t.clips)
            c.id = nextClipId_++;
        sortClips(t);
    }
    pruneHost();
}

void AudioTracks::restoreEdits(const AudioTracks& snapshot)
{
    const int nextTrackId = std::max(nextTrackId_, snapshot.nextTrackId_);
    const int nextClipId = std::max(nextClipId_, snapshot.nextClipId_);
    *this = snapshot;
    nextTrackId_ = nextTrackId;
    nextClipId_ = nextClipId;
}

void AudioTracks::clear()
{
    original_ = {};
    tracks_.clear();
}

double AudioTracks::endTime() const
{
    double end = 0.0;
    for (const AudioTrack& t : tracks_) {
        for (const AudioClip& c : t.clips)
            end = std::max(end, c.endPosition());
    }
    return end;
}

}  // namespace editor::core
