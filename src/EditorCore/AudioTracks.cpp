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

bool AudioTracks::setPan(int trackId, double pan)
{
    AudioMix* m = mutableMix(trackId);
    if (m == nullptr || !std::isfinite(pan))
        return false;
    pan = std::clamp(pan, -1.0, 1.0);
    if (m->pan == pan)
        return false;
    m->pan = pan;
    return true;
}

std::pair<float, float> AudioTracks::panGains(double pan)
{
    pan = std::clamp(pan, -1.0, 1.0);
    constexpr double kHalfPi = 1.57079632679489661923;
    const float falling = pan == 1.0 || pan == -1.0 ? 0.0f : static_cast<float>(std::cos(std::abs(pan) * kHalfPi));
    return pan > 0.0 ? std::pair<float, float>{falling, 1.0f} : std::pair<float, float>{1.0f, falling};
}

bool AudioTracks::setMasterLevel(double db)
{
    if (!std::isfinite(db))
        return false;
    db = std::clamp(db, kMinLevelDb, kMaxLevelDb);
    if (masterDb_ == db)
        return false;
    masterDb_ = db;
    return true;
}

const std::vector<AudioEffect>& AudioTracks::effects(int trackId) const
{
    static const std::vector<AudioEffect> none;
    if (trackId == kMasterTrack)
        return masterEffects_;
    if (trackId == kOriginalTrack)
        return original_.effects;
    const AudioTrack* t = track(trackId);
    return t == nullptr ? none : t->mix.effects;
}

bool AudioTracks::setEffects(int trackId, std::vector<AudioEffect> effects)
{
    for (AudioEffect& e : effects)
        e = clampedEffect(e);
    std::vector<AudioEffect>* target = nullptr;
    if (trackId == kMasterTrack)
        target = &masterEffects_;
    else if (AudioMix* m = mutableMix(trackId))
        target = &m->effects;
    if (target == nullptr || *target == effects)
        return false;
    *target = std::move(effects);
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
    const auto anyActive = [](const std::vector<AudioEffect>& list) {
        return std::any_of(list.begin(), list.end(), [](const AudioEffect& e) { return e.active(); });
    };
    if (!audible(kOriginalTrack) || original_.levelDb != 0.0 || original_.pan != 0.0 || masterDb_ != 0.0
        || anyActive(original_.effects) || anyActive(masterEffects_))
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

namespace {

// La traccia con in più, al posto dei tratti occupati da altro, clip finte (id negativi: non coincidono con nessuna).
AudioTrack withBlocked(const AudioTrack& track, const std::vector<TimeRange>& blocked)
{
    AudioTrack result = track;
    int id = -1;
    for (const TimeRange& r : blocked) {
        AudioClip fake;
        fake.id = id--;
        fake.end = r.length();
        fake.position = r.start;
        result.clips.push_back(fake);
    }
    return result;
}

}  // namespace

double AudioTracks::nearestFreePosition(int clipId, int trackId, double desired, const std::vector<TimeRange>& blocked) const
{
    const AudioClip* clip = find(clipId);
    static const AudioTrack kNoClips;
    const AudioTrack* t = trackId == kOriginalTrack && !hasHost() ? &kNoClips : track(trackId);
    if (clip == nullptr || t == nullptr)
        return -1.0;
    if (!blocked.empty())
        return freeSpot(withBlocked(*t, blocked), clip->length(), desired, clipId);
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
    std::vector<TimeRange> occupied;
    for (const AudioClip& other : track.clips) {
        if (other.id != ignoredClip)
            occupied.push_back({other.position, other.endPosition()});
    }
    return closestFreeStart(std::move(occupied), length, desired, kTimeEpsilon);
}

bool AudioTracks::moveTo(int clipId, int trackId, double position, const std::vector<TimeRange>& blocked)
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
    if (overlaps(*to, position, clip.length(), clipId) || overlaps(withBlocked(AudioTrack{}, blocked), position, clip.length(), clipId)) {
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
        const SpeedMap map = first.map();
        const double cutAt = seconds - first.position;
        const TimeRange firstPart = map.fileRangeOf(0.0, cutAt);
        const TimeRange secondPart = map.fileRangeOf(cutAt, map.length());
        second.start = secondPart.start;
        second.end = secondPart.end;
        second.position = seconds;
        first.start = firstPart.start;
        first.end = firstPart.end;
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
    // Senza ripple il bordo si sposta nel montaggio: a sinistra non prima di 0 né della fine della clip precedente, a destra non
    // oltre l'inizio della clip dopo. Lo spazio libero (secondi del montaggio) nel file è `room * speed`; con l'inverso il bordo
    // Start del file sta a destra.
    const bool onLeft = (edge == Sequence::Edge::Start) != c.reverse;
    double room = std::numeric_limits<double>::max();
    if (!ripple) {
        if (onLeft) {
            const double previousEnd = index > 0 ? clips[static_cast<std::size_t>(index) - 1].endPosition() : 0.0;
            room = c.position - previousEnd;
        } else if (static_cast<std::size_t>(index) + 1 < clips.size()) {
            room = clips[static_cast<std::size_t>(index) + 1].position - c.endPosition();
        }
    }
    const double roomInFile = room >= std::numeric_limits<double>::max() ? room : room * c.speed;
    const double minFileLength = minLength * c.speed;
    if (edge == Sequence::Edge::Start) {
        const double lowest = ripple ? 0.0 : std::max(0.0, c.start - roomInFile);
        return std::clamp(desired, std::min(lowest, c.start), std::max(c.start, c.end - minFileLength));
    }
    double highest = c.sourceDuration > 0 ? c.sourceDuration : std::numeric_limits<double>::max();
    if (!ripple && roomInFile < std::numeric_limits<double>::max())
        highest = std::min(highest, c.end + roomInFile);
    return std::clamp(desired, std::min(c.start + minFileLength, c.end), std::max(highest, c.end));
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
    // Quanto si sposta il bordo nel montaggio (con l'inverso il verso è opposto) e se sta a sinistra o a destra.
    const bool onLeft = (edge == Sequence::Edge::Start) != c.reverse;
    const double delta = (value - old) / c.speed * (c.reverse ? -1.0 : 1.0);
    c.envelope = envelopeForTrim(c.envelope, edge == Sequence::Edge::Start, old, value, edge == Sequence::Edge::Start ? c.end : c.start);
    double shift = 0.0;  // di quanto scorrono le clip dopo (con ripple)
    if (edge == Sequence::Edge::Start)
        c.start = value;
    else
        c.end = value;
    if (onLeft) {
        if (ripple)
            shift = -delta;
        else
            c.position = std::max(0.0, c.position + delta);  // la fine resta ferma nella sequenza
    } else if (ripple) {
        shift = delta;
    }
    for (std::size_t i = static_cast<std::size_t>(index) + 1; i < clips.size(); ++i)
        clips[i].position = std::max(0.0, clips[i].position + shift);
    return true;
}

bool AudioTracks::setAudioSpeed(int clipId, const AudioSpeedOptions& options)
{
    int t = 0;
    int index = 0;
    if (!locate(clipId, t, index))
        return false;
    AudioClip& c = tracks_[static_cast<std::size_t>(t)].clips[static_cast<std::size_t>(index)];
    if (c.audioSpeed == options)
        return false;
    c.audioSpeed = options;
    return true;
}

bool AudioTracks::setSpeed(int clipId, double speed, bool reverse)
{
    int t = 0;
    int index = 0;
    if (!locate(clipId, t, index))
        return false;
    AudioClip& c = tracks_[static_cast<std::size_t>(t)].clips[static_cast<std::size_t>(index)];
    speed = clampSpeed(speed);
    if (std::abs(c.speed - speed) < 1e-9 && c.reverse == reverse)
        return false;
    if (overlaps(tracks_[static_cast<std::size_t>(t)], c.position, (c.end - c.start) / speed, clipId))
        return false;
    c.speed = speed;
    c.reverse = reverse;
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
            const TimeRange part = c.map().fileRangeOf(0.0, from - c.position);
            before.start = part.start;
            before.end = part.end;
            kept.push_back(before);
            keptId = true;
        }
        if (c.endPosition() > to + kTimeEpsilon) {  // resta la parte dopo
            AudioClip after = c;
            if (keptId)
                after.id = nextClipId_++;
            const TimeRange part = c.map().fileRangeOf(to - c.position, c.length());
            after.start = part.start;
            after.end = part.end;
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

void AudioTracks::restore(AudioMix original, std::vector<AudioTrack> tracks, double masterDb,
                          std::vector<AudioEffect> masterEffects)
{
    original_ = original;
    masterEffects_ = std::move(masterEffects);
    for (AudioEffect& e : masterEffects_)
        e = clampedEffect(e);
    for (AudioEffect& e : original_.effects)
        e = clampedEffect(e);
    masterDb_ = std::clamp(masterDb, kMinLevelDb, kMaxLevelDb);
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
    masterDb_ = 0.0;
    masterEffects_.clear();
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
