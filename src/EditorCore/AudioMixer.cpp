#include "EditorCore/AudioMixer.h"

#include "EditorCore/AudioDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace editor::core {

namespace {

std::vector<MixSegment> mixSegmentsOf(const Project& project, const Sequence& sequence)
{
    std::vector<MixSegment> segments;
    const std::vector<double> starts = sequence.starts();
    const auto& fragments = sequence.fragments();
    std::vector<int> segmentOfFragment(fragments.size(), -1);
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        const Fragment& f = fragments[i];
        const Clip* clip = project.find(f.clipId);
        if (clip == nullptr || f.length() <= 0.0)
            continue;
        segmentOfFragment[i] = static_cast<int>(segments.size());
        MixSegment s;
        s.path = clip->path;
        s.fileStart = f.start;
        s.fileEnd = f.end;
        s.position = starts[i];
        s.trackId = AudioTracks::kOriginalTrack;
        s.ofPiece = true;
        s.muted = f.muted;
        s.envelope = f.envelope;
        segments.push_back(std::move(s));
    }
    // Transizioni con la dissolvenza dell'audio: il pezzo prima continua oltre la giunzione e quello dopo comincia
    // prima (le "maniglie" della transizione), e i due si incrociano a potenza costante su tutta la durata.
    for (const TransitionPlacement& t : project.activeTransitions()) {
        const TransitionClip* clip = project.transitions().find(t.id);
        if (clip == nullptr || !clip->crossfadeAudio)
            continue;
        const int indexA = sequence.indexOf(t.beforeFragment);
        const int indexB = sequence.indexOf(t.afterFragment);
        if (indexA < 0 || indexB < 0)
            continue;
        const int a = segmentOfFragment[static_cast<std::size_t>(indexA)];
        const int b = segmentOfFragment[static_cast<std::size_t>(indexB)];
        if (a < 0 || b < 0)
            continue;
        MixSegment& before = segments[static_cast<std::size_t>(a)];
        MixSegment& after = segments[static_cast<std::size_t>(b)];
        before.fileEnd += t.after;
        before.fadeOut = t.duration();
        after.fileStart -= t.before;
        after.position -= t.before;
        after.fadeIn = t.duration();
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
            s.envelope = clip.envelope;
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

namespace {

// I tratti delle tracce aggiunte quando gli spazi vuoti del montaggio si chiudono: come per i pezzi video, il tempo degli
// spazi si toglie da tutte le tracce, quindi la parte di una clip dentro uno spazio sparisce e il resto scorre indietro.
std::vector<MixSegment> closeGapsInAddedTracks(const std::vector<MixSegment>& segments, const std::vector<TimeRange>& gaps)
{
    std::vector<MixSegment> result;
    for (const MixSegment& s : segments) {
        if (s.ofPiece) {  // già al suo posto: i pezzi video sono nella sequenza senza spazi
            result.push_back(s);
            continue;
        }
        // Le parti di s fuori dagli spazi, nei tempi della sequenza di prima.
        double from = s.position;
        const double end = s.position + s.length();
        double removed = 0.0;  // somma degli spazi prima di `from`
        const auto emit = [&](double a, double b) {
            if (b - a <= 1e-6)
                return;
            MixSegment part = s;
            part.fileStart = s.fileStart + (a - s.position);
            part.fileEnd = s.fileStart + (b - s.position);
            part.position = a - removed;
            result.push_back(std::move(part));
        };
        double gapsBefore = 0.0;
        for (const TimeRange& g : gaps) {
            if (g.end <= from) {
                gapsBefore += g.length();
                continue;
            }
            if (g.start >= end)
                break;
            removed = gapsBefore;
            emit(from, std::min(g.start, end));
            gapsBefore += g.length();
            from = std::max(from, g.end);
            removed = gapsBefore;
        }
        removed = gapsBefore;
        if (from < end)
            emit(from, end);
    }
    return result;
}

}  // namespace

MixSpec mixSpecOf(const Project& project, bool closeGaps)
{
    Sequence sequence = project.sequence();
    std::vector<TimeRange> gaps;
    if (closeGaps) {
        double start = 0.0;
        for (const Fragment& f : sequence.fragments()) {
            if (f.gap > 0.0)
                gaps.push_back({start, start + f.gap});
            start += f.gap + f.length();
        }
        sequence.closeAllGaps();
    }
    MixSpec spec;
    spec.segments = mixSegmentsOf(project, sequence);
    if (closeGaps && !gaps.empty()) {
        spec.segments = closeGapsInAddedTracks(spec.segments, gaps);
        std::stable_sort(spec.segments.begin(), spec.segments.end(),
                         [](const MixSegment& a, const MixSegment& b) { return a.position < b.position; });
    }
    spec.gains = trackGainsOf(project.audioTracks());
    spec.pans = trackPansOf(project.audioTracks());
    spec.masterGain = static_cast<float>(project.audioTracks().masterGain());
    spec.effects = trackEffectsOf(project.audioTracks());
    spec.length = sequence.totalLength();
    if (!closeGaps) {
        for (const MixSegment& s : spec.segments)
            spec.length = std::max(spec.length, s.position + s.length());  // le clip audio che vanno oltre l'ultimo pezzo
    }
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

std::map<int, float> trackPansOf(const AudioTracks& tracks)
{
    std::map<int, float> pans;
    pans[AudioTracks::kOriginalTrack] = static_cast<float>(tracks.mix(AudioTracks::kOriginalTrack).pan);
    for (const AudioTrack& track : tracks.tracks())
        pans[track.id] = static_cast<float>(track.mix.pan);
    return pans;
}

std::map<int, std::vector<AudioEffect>> trackEffectsOf(const AudioTracks& tracks)
{
    std::map<int, std::vector<AudioEffect>> effects;
    const auto add = [&](int id) {
        std::vector<AudioEffect> active;
        for (const AudioEffect& e : tracks.effects(id)) {
            if (e.active())
                active.push_back(e);
        }
        if (!active.empty())
            effects[id] = std::move(active);
    };
    add(AudioTracks::kOriginalTrack);
    for (const AudioTrack& track : tracks.tracks())
        add(track.id);
    add(AudioTracks::kMasterTrack);
    return effects;
}

std::vector<std::vector<LevelPoint>> segmentEnvelopesOf(const Project& project)
{
    std::vector<std::vector<LevelPoint>> envelopes;
    for (const MixSegment& s : mixSegmentsOf(project))
        envelopes.push_back(s.envelope);
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
              const std::vector<LevelPoint>& envelope, float* peak, const std::array<float, 4>& pan)
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
        const long long fadeInFrames = std::llround(segment.fadeIn * kMixSampleRate);
        const long long fadeOutFrames = std::llround(segment.fadeOut * kMixSampleRate);
        const long long segmentFrames = std::max<long long>(1, std::llround(segment.length() * kMixSampleRate));
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
            if (!envelope.empty()) {
                const double fileTime = segment.fileStart + static_cast<double>(frame - segmentFirst) / kMixSampleRate;
                gain *= static_cast<float>(envelopeGain(envelopeDbAt(envelope, fileTime)));
            }
            if (fadeInFrames > 0 || fadeOutFrames > 0) {
                const long long rel = frame - segmentFirst;
                if (fadeInFrames > 0 && rel < fadeInFrames)
                    gain *= std::sin(1.5707963f * static_cast<float>(rel) / static_cast<float>(fadeInFrames));
                const long long left = segmentFrames - rel;
                if (fadeOutFrames > 0 && left < fadeOutFrames)
                    gain *= std::sin(1.5707963f * static_cast<float>(left) / static_cast<float>(fadeOutFrames));
            }
            const std::size_t src = (pendingPos + i) * kMixChannels;
            // Pan: sinistra e destra con la loro rampa (pan[0], pan[1] all'inizio del blocco, pan[2], pan[3] alla fine).
            const float panLeft = pan[0] + (pan[2] - pan[0]) * t;
            const float panRight = pan[1] + (pan[3] - pan[1]) * t;
            const float left = pending[src] * (1.0f / 32768.0f) * gain * panLeft;
            const float right = pending[src + 1] * (1.0f / 32768.0f) * gain * panRight;
            acc[at * 2] += left;
            acc[at * 2 + 1] += right;
            peak[0] = std::max(peak[0], std::abs(left));
            peak[1] = std::max(peak[1], std::abs(right));
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
    std::map<int, float> pans;
    float masterGain = 1.0f;
    std::map<int, std::vector<AudioEffect>> effects;  // effetti attivi per traccia (kMasterTrackId = Master)
    std::vector<std::vector<LevelPoint>> envelopes;  // uno per tratto
    std::map<int, std::array<float, 2>> peaks;       // picchi (sinistro, destro) per traccia dall'ultimo takePeaks
};

AudioMixer::AudioMixer(std::vector<MixSegment> segments, std::map<int, float> gains, std::map<int, float> pans, float masterGain,
                       std::map<int, std::vector<AudioEffect>> effects)
    : segments_(std::move(segments))
    , shared_(std::make_unique<Shared>())
{
    shared_->gains = std::move(gains);
    applied_ = shared_->gains;
    shared_->pans = std::move(pans);
    appliedPans_ = shared_->pans;
    shared_->masterGain = masterGain;
    shared_->effects = std::move(effects);
    appliedMaster_ = masterGain;
    for (const MixSegment& s : segments_) {
        shared_->envelopes.push_back(s.envelope);
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

std::map<int, std::array<float, 2>> AudioMixer::takePeaks()
{
    std::lock_guard lock(shared_->mutex);
    std::map<int, std::array<float, 2>> taken;
    taken.swap(shared_->peaks);
    return taken;
}

void AudioMixer::setEffects(std::map<int, std::vector<AudioEffect>> effects)
{
    std::lock_guard lock(shared_->mutex);
    shared_->effects = std::move(effects);
}

void AudioMixer::setMasterGain(float gain)
{
    std::lock_guard lock(shared_->mutex);
    shared_->masterGain = gain;
}

void AudioMixer::setPans(const std::map<int, float>& pans)
{
    std::lock_guard lock(shared_->mutex);
    shared_->pans = pans;
}

void AudioMixer::setGains(const std::map<int, float>& gains)
{
    std::lock_guard lock(shared_->mutex);
    shared_->gains = gains;
}

void AudioMixer::setEnvelopes(std::vector<std::vector<LevelPoint>> envelopes)
{
    std::lock_guard lock(shared_->mutex);
    if (envelopes.size() == shared_->envelopes.size())
        shared_->envelopes = std::move(envelopes);
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
    std::map<int, float> panTargets;
    float masterTarget = 1.0f;
    std::map<int, std::vector<AudioEffect>> effectTargets;
    std::vector<std::vector<LevelPoint>> envelopes;
    {
        std::lock_guard lock(shared_->mutex);
        targets = shared_->gains;
        panTargets = shared_->pans;
        masterTarget = shared_->masterGain;
        effectTargets = shared_->effects;
        envelopes = shared_->envelopes;
    }
    std::vector<float>& acc = acc_;  // riusato a ogni blocco: niente allocazioni sul thread audio
    acc.assign(frames * 2, 0.0f);
    const long long blockEnd = firstFrame + static_cast<long long>(frames);
    std::map<int, std::array<float, 2>> blockPeaks;  // picco sinistro e destro di ogni traccia in questo blocco
    // Equalizzatori: una traccia con un effetto attivo si somma prima in un suo buffer, poi si filtra e si aggiunge al mix.
    if (firstFrame != nextExpected_) {
        for (auto& [id, state] : eq_) {
            for (EqualizerFilter& filter : state.filters)
                filter.reset();  // un salto: lo stato dei filtri non vale più
        }
    }
    nextExpected_ = blockEnd;
    const auto isEqualizer = [](const AudioEffect& e) { return e.kind == EffectKind::Equalizer; };
    // Passa `data` in tutti gli equalizzatori attivi della lista, nell'ordine (uno stato per equalizzatore).
    const auto equalize = [&](int id, const std::vector<AudioEffect>& list, float* data) {
        EqState& state = eq_[id];
        std::size_t n = 0;
        for (const AudioEffect& e : list) {
            if (!isEqualizer(e))
                continue;
            if (state.filters.size() <= n)
                state.filters.emplace_back();
            state.filters[n].setBands(e.bands, kMixSampleRate);
            state.filters[n].process(data, frames);
            ++n;
        }
    };
    std::map<int, std::vector<float>*> trackBuffers;
    for (const auto& [id, list] : effectTargets) {
        if (id == kMasterTrackId || !std::any_of(list.begin(), list.end(), isEqualizer))
            continue;
        std::vector<float>& buffer = eq_[id].buffer;
        buffer.assign(frames * 2, 0.0f);
        trackBuffers[id] = &buffer;
    }
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
        const auto panTo = panTargets.find(segment.trackId);
        const auto panFrom = appliedPans_.find(segment.trackId);
        const float p1 = panTo != panTargets.end() ? panTo->second : 0.0f;
        const float p0 = panFrom != appliedPans_.end() ? panFrom->second : 0.0f;  // senza pan noto: era al centro
        const auto [l0, r0] = AudioTracks::panGains(p0);
        const auto [l1, r1] = AudioTracks::panGains(p1);
        const long long from = std::max(firstFrame, segFirst);
        const long long to = std::min(blockEnd, segEnd);
        std::vector<std::pair<long long, long long>> mutedFrames;
        for (const TimeRange& m : segment.muted) {
            mutedFrames.emplace_back(segFirst + static_cast<long long>(std::ceil((m.start - segment.fileStart) * kMixSampleRate)),
                                     segFirst + static_cast<long long>(std::ceil((m.end - segment.fileStart) * kMixSampleRate)));
        }
        const auto buffer = trackBuffers.find(segment.trackId);
        float unusedPeak[2] = {0.0f, 0.0f};  // con un effetto il picco si misura dopo, sul suono filtrato
        readers_[i]->read(segment, segFirst, from, static_cast<std::size_t>(to - from),
                          buffer != trackBuffers.end() ? *buffer->second : acc, static_cast<std::size_t>(from - firstFrame),
                          frames, g0, g1, mutedFrames, envelopes[i],
                          buffer != trackBuffers.end() ? unusedPeak : blockPeaks[segment.trackId].data(), {l0, r0, l1, r1});
    }
    for (const auto& [id, buffer] : trackBuffers) {
        equalize(id, effectTargets[id], buffer->data());
        std::array<float, 2>& peak = blockPeaks[id];
        for (std::size_t i = 0; i < frames; ++i) {
            peak[0] = std::max(peak[0], std::abs((*buffer)[i * 2]));
            peak[1] = std::max(peak[1], std::abs((*buffer)[i * 2 + 1]));
            acc[i * 2] += (*buffer)[i * 2];
            acc[i * 2 + 1] += (*buffer)[i * 2 + 1];
        }
    }
    // L'equalizzatore del Master agisce su tutto il mix, prima del suo livello.
    if (const auto master = effectTargets.find(kMasterTrackId); master != effectTargets.end())
        equalize(kMasterTrackId, master->second, acc.data());
    for (const auto& [trackId, gain] : targets)
        applied_[trackId] = gain;
    for (const auto& [trackId, pan] : panTargets)
        appliedPans_[trackId] = pan;
    // Il Master: moltiplica tutto il mix (con la rampa) e se ne misura il picco, prima del limitatore.
    {
        std::array<float, 2>& masterPeak = blockPeaks[kMasterTrackId];
        const float m0 = appliedMaster_;
        for (std::size_t i = 0; i < frames; ++i) {
            const float t = frames > 1 ? static_cast<float>(i) / static_cast<float>(frames) : 1.0f;
            const float g = m0 + (masterTarget - m0) * t;
            if (g != 1.0f) {
                acc[i * 2] *= g;
                acc[i * 2 + 1] *= g;
            }
            masterPeak[0] = std::max(masterPeak[0], std::abs(acc[i * 2]));
            masterPeak[1] = std::max(masterPeak[1], std::abs(acc[i * 2 + 1]));
        }
        appliedMaster_ = masterTarget;
    }
    {
        std::lock_guard lock(shared_->mutex);
        for (const auto& [trackId, peak] : blockPeaks) {
            std::array<float, 2>& kept = shared_->peaks[trackId];
            kept[0] = std::max(kept[0], peak[0]);
            kept[1] = std::max(kept[1], peak[1]);
        }
    }

    out.resize(frames * 2);
    for (std::size_t i = 0; i < frames * 2; ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(std::lround(limit(acc[i]) * 32768.0f), -32768L, 32767L));
}

}  // namespace editor::core
