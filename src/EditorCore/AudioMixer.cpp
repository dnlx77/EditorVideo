#include "EditorCore/AudioMixer.h"

#include "EditorCore/AudioDecoder.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

namespace {

std::vector<MixSegment> mixSegmentsOf(const Project& project, const Sequence& sequence)
{
    std::vector<MixSegment> segments;
    const std::vector<double> starts = sequence.starts();
    const auto& fragments = sequence.fragments();
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        const Fragment& f = fragments[i];
        const Clip* clip = project.find(f.clipId);
        if (clip == nullptr || f.length() <= 0.0)
            continue;
        MixSegment s;
        s.path = clip->path;
        s.fileStart = f.start;
        s.fileEnd = f.end;
        s.position = starts[i];
        s.trackId = AudioTracks::kOriginalTrack;
        s.muted = f.muted;
        segments.push_back(std::move(s));
    }
    for (const AudioTrack& track : project.audioTracks().tracks()) {
        for (const AudioClip& clip : track.clips) {
            if (clip.length() <= 0.0)
                continue;
            MixSegment s;
            s.path = clip.path;
            s.fileStart = clip.start;
            s.fileEnd = clip.end;
            s.position = clip.position;
            s.trackId = track.id;
            segments.push_back(std::move(s));
        }
    }
    std::stable_sort(segments.begin(), segments.end(),
                     [](const MixSegment& a, const MixSegment& b) { return a.position < b.position; });
    return segments;
}

}  // namespace

std::vector<MixSegment> mixSegmentsOf(const Project& project)
{
    return mixSegmentsOf(project, project.sequence());
}

MixSpec mixSpecOf(const Project& project, bool closeGaps)
{
    Sequence sequence = project.sequence();
    if (closeGaps)
        sequence.closeAllGaps();
    MixSpec spec;
    spec.segments = mixSegmentsOf(project, sequence);
    spec.gains = trackGainsOf(project.audioTracks());
    spec.envelopes = trackEnvelopesOf(project.audioTracks());
    spec.length = sequence.totalLength();
    return spec;
}

std::map<int, float> trackGainsOf(const AudioTracks& tracks)
{
    std::map<int, float> gains;
    gains[AudioTracks::kOriginalTrack] = static_cast<float>(tracks.gain(AudioTracks::kOriginalTrack));
    for (const AudioTrack& track : tracks.tracks())
        gains[track.id] = static_cast<float>(tracks.gain(track.id));
    return gains;
}

TrackEnvelopes trackEnvelopesOf(const AudioTracks& tracks)
{
    TrackEnvelopes envelopes;
    if (!tracks.envelope(AudioTracks::kOriginalTrack).empty())
        envelopes[AudioTracks::kOriginalTrack] = tracks.envelope(AudioTracks::kOriginalTrack);
    for (const AudioTrack& track : tracks.tracks()) {
        if (!track.mix.envelope.empty())
            envelopes[track.id] = track.mix.envelope;
    }
    return envelopes;
}

// Il file di un tratto, letto in avanti: ricorda dove è arrivato e si riposiziona solo se il blocco richiesto non
// segue quello di prima.
struct AudioMixer::Reader
{
    std::unique_ptr<AudioDecoder> decoder;
    bool failed = false;
    bool ended = false;                 // il file è finito prima della fine del tratto: il resto è silenzio
    long long nextFrame = -1;          // fotogramma della sequenza del prossimo campione di `pending`
    std::vector<std::int16_t> pending;  // stereo interlacciato, già alla frequenza del mix
    std::size_t pendingPos = 0;         // fotogrammi di `pending` già dati

    std::size_t available() const { return pending.size() / kMixChannels - pendingPos; }

    // Aggiunge ad `acc` (stereo, float) `count` fotogrammi del tratto a partire dal fotogramma `from` della sequenza,
    // dalla posizione `offset` di acc; con il livello che va da g0 a g1 in modo lineare su tutta la lunghezza di `total`
    // (la rampa è calcolata sul blocco intero, non sulla parte che riguarda questo tratto).
    void read(const MixSegment& segment, long long segmentFirst, long long from, std::size_t count,
              std::vector<float>& acc, std::size_t offset, std::size_t totalFrames, float g0, float g1,
              const std::vector<std::pair<long long, long long>>& mutedFrames,
              const std::vector<LevelPoint>* envelope)
    {
        if (failed)
            return;
        if (!decoder) {
            try {
                decoder = std::make_unique<AudioDecoder>(segment.path, kMixSampleRate);
            } catch (const std::exception&) {
                failed = true;  // niente audio in questo file: silenzio
                return;
            }
        }
        if (nextFrame != from) {
            try {
                decoder->seek(segment.fileStart + static_cast<double>(from - segmentFirst) / kMixSampleRate);
            } catch (const std::exception&) {
                failed = true;
                return;
            }
            pending.clear();
            pendingPos = 0;
            nextFrame = from;
            ended = false;
        }
        try {
            while (!ended && available() < count) {
                auto chunk = decoder->next();
                if (!chunk) {
                    ended = true;
                    break;
                }
                if (pendingPos > 0 && pendingPos >= pending.size() / kMixChannels / 2) {
                    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pendingPos) * kMixChannels);
                    pendingPos = 0;
                }
                if (decoder->channels() == 1) {
                    for (std::int16_t v : chunk->samples) {
                        pending.push_back(v);
                        pending.push_back(v);
                    }
                } else {
                    pending.insert(pending.end(), chunk->samples.begin(), chunk->samples.end());
                }
            }
        } catch (const std::exception&) {
            failed = true;
        }
        const std::size_t have = std::min(count, available());
        for (std::size_t i = 0; i < have; ++i) {
            const long long frame = from + static_cast<long long>(i);
            bool muted = false;
            for (const auto& [mutedFrom, mutedTo] : mutedFrames) {
                if (frame >= mutedFrom && frame < mutedTo) {
                    muted = true;
                    break;
                }
            }
            if (muted)
                continue;
            const std::size_t at = offset + i;
            const float t = totalFrames > 1 ? static_cast<float>(at) / static_cast<float>(totalFrames) : 1.0f;
            float gain = g0 + (g1 - g0) * t;
            if (envelope != nullptr) {
                gain *= static_cast<float>(
                    AudioTracks::envelopeGain(AudioTracks::envelopeDbAt(*envelope, static_cast<double>(frame) / kMixSampleRate)));
            }
            const std::size_t src = (pendingPos + i) * kMixChannels;
            acc[at * 2] += pending[src] * (1.0f / 32768.0f) * gain;
            acc[at * 2 + 1] += pending[src + 1] * (1.0f / 32768.0f) * gain;
        }
        // Anche se il file è finito prima (o non si legge più): si va avanti, il resto è silenzio.
        pendingPos += have;
        nextFrame = from + static_cast<long long>(count);
        if (have < count) {
            pending.clear();
            pendingPos = 0;
        }
    }
};

struct AudioMixer::Shared
{
    std::mutex mutex;
    std::map<int, float> gains;
    TrackEnvelopes envelopes;
};

AudioMixer::AudioMixer(std::vector<MixSegment> segments, std::map<int, float> gains, TrackEnvelopes envelopes)
    : segments_(std::move(segments))
    , shared_(std::make_unique<Shared>())
{
    shared_->gains = std::move(gains);
    shared_->envelopes = std::move(envelopes);
    applied_ = shared_->gains;
    for (const MixSegment& s : segments_) {
        first_.push_back(std::llround(s.position * kMixSampleRate));
        length_.push_back(std::max<long long>(1, std::llround(s.length() * kMixSampleRate)));
        readers_.push_back(std::make_unique<Reader>());
        end_ = std::max(end_, s.position + s.length());
    }
}

AudioMixer::~AudioMixer() = default;
AudioMixer::AudioMixer(AudioMixer&&) noexcept = default;
AudioMixer& AudioMixer::operator=(AudioMixer&&) noexcept = default;

void AudioMixer::setGain(int trackId, float gain)
{
    std::lock_guard lock(shared_->mutex);
    shared_->gains[trackId] = gain;
}

void AudioMixer::setGains(const std::map<int, float>& gains)
{
    std::lock_guard lock(shared_->mutex);
    shared_->gains = gains;
}

void AudioMixer::setEnvelopes(const TrackEnvelopes& envelopes)
{
    std::lock_guard lock(shared_->mutex);
    shared_->envelopes = envelopes;
}

float AudioMixer::limit(float value)
{
    constexpr float knee = 0.95f;
    const float magnitude = std::abs(value);
    if (magnitude <= knee)
        return value;
    const float room = 1.0f - knee;
    const float limited = knee + room * std::tanh((magnitude - knee) / room);
    return value < 0 ? -limited : limited;
}

void AudioMixer::render(long long firstFrame, std::size_t frames, std::vector<std::int16_t>& out)
{
    std::map<int, float> targets;
    TrackEnvelopes envelopes;
    {
        std::lock_guard lock(shared_->mutex);
        targets = shared_->gains;
        envelopes = shared_->envelopes;
    }
    std::vector<float> acc(frames * 2, 0.0f);
    const long long blockEnd = firstFrame + static_cast<long long>(frames);
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const long long segFirst = first_[i];
        const long long segEnd = segFirst + length_[i];
        if (segEnd <= firstFrame) {
            readers_[i]->decoder.reset();  // già passato: il file si chiude
            readers_[i]->nextFrame = -1;
            readers_[i]->pending.clear();
            readers_[i]->pendingPos = 0;
            continue;
        }
        if (segFirst >= blockEnd)
            continue;
        const MixSegment& segment = segments_[i];
        const auto target = targets.find(segment.trackId);
        const float g1 = target != targets.end() ? target->second : 1.0f;
        const auto before = applied_.find(segment.trackId);
        const float g0 = before != applied_.end() ? before->second : g1;
        if (g0 == 0.0f && g1 == 0.0f)
            continue;  // traccia che non si sente: nemmeno si legge
        const long long from = std::max(firstFrame, segFirst);
        const long long to = std::min(blockEnd, segEnd);
        std::vector<std::pair<long long, long long>> mutedFrames;
        for (const TimeRange& m : segment.muted) {
            mutedFrames.emplace_back(segFirst + static_cast<long long>(std::ceil((m.start - segment.fileStart) * kMixSampleRate)),
                                     segFirst + static_cast<long long>(std::ceil((m.end - segment.fileStart) * kMixSampleRate)));
        }
        const auto envelope = envelopes.find(segment.trackId);
        readers_[i]->read(segment, segFirst, from, static_cast<std::size_t>(to - from), acc,
                          static_cast<std::size_t>(from - firstFrame), frames, g0, g1, mutedFrames,
                          envelope != envelopes.end() ? &envelope->second : nullptr);
    }
    for (const auto& [trackId, gain] : targets)
        applied_[trackId] = gain;

    out.resize(frames * 2);
    for (std::size_t i = 0; i < frames * 2; ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(std::lround(limit(acc[i]) * 32768.0f), -32768L, 32767L));
}

}  // namespace editor::core
