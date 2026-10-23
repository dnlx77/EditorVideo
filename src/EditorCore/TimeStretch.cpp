#include "EditorCore/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace editor::core {

namespace {

std::int16_t toSample(float value)
{
    return static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
}

}  // namespace

TimeStretch::TimeStretch(int channels, int sampleRate, double speed)
    : channels_(std::max(1, channels))
    , speed_(speed > 0 ? speed : 1.0)
{
    const auto rate = static_cast<std::size_t>(std::max(8000, sampleRate));
    window_ = std::max<std::size_t>(64, rate * 30 / 1000) & ~std::size_t{1};  // ~30 ms, pari
    hop_ = window_ / 2;
    search_ = rate * 12 / 1000;  // ±12 ms
    // Hann "periodica": due pezzi sovrapposti a metà sommano esattamente a 1.
    hann_.resize(window_);
    for (std::size_t i = 0; i < window_; ++i)
        hann_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / window_));
    overlap_.assign(hop_ * channels_, 0.0f);
}

void TimeStretch::push(const std::int16_t* samples, std::size_t count)
{
    input_.insert(input_.end(), samples, samples + count);
}

std::size_t TimeStretch::bestStart(std::size_t nominal, std::size_t previous) const
{
    // Il pezzo ideale è la continuazione naturale del precedente (previous + hop): si cerca, intorno al punto
    // previsto, l'inizio più simile. Per fare presto si confronta la somma dei canali, un campione ogni 4.
    const std::size_t natural = previous + hop_;
    const std::size_t from = nominal > search_ ? nominal - search_ : 0;
    const std::size_t to = nominal + search_;
    const auto mono = [&](std::size_t frame) {
        float sum = 0.0f;
        for (int c = 0; c < channels_; ++c)
            sum += input_[frame * channels_ + c];
        return sum;
    };
    std::size_t best = nominal;
    double bestScore = -1e300;
    for (std::size_t start = from; start <= to; start += 2) {
        double dot = 0.0;
        double energy = 1e-9;
        for (std::size_t i = 0; i < window_; i += 4) {
            const float a = mono(start + i);
            dot += static_cast<double>(a) * mono(natural + i);
            energy += static_cast<double>(a) * a;
        }
        const double score = dot / std::sqrt(energy);
        if (score > bestScore) {
            bestScore = score;
            best = start;
        }
    }
    return best;
}

void TimeStretch::pull(std::vector<std::int16_t>& out)
{
    if (speed_ == 1.0) {
        for (float v : input_)
            out.push_back(toSample(v));
        input_.clear();
        return;
    }
    for (;;) {
        const auto nominal = static_cast<std::size_t>(std::llround(nominal_));
        std::size_t start = nominal;
        if (!first_) {
            // Servono tutti i campioni che la ricerca può guardare.
            const std::size_t needed = std::max(nominal + search_, previous_ + hop_) + window_ + 1;
            if (inputFrames() < needed)
                break;
            start = bestStart(nominal, previous_);
        } else if (inputFrames() < start + window_) {
            break;
        }
        // Prima metà del pezzo + seconda metà del precedente = hop campioni finiti.
        for (std::size_t i = 0; i < hop_; ++i) {
            for (int c = 0; c < channels_; ++c) {
                const std::size_t o = i * channels_ + c;
                out.push_back(toSample(overlap_[o] + hann_[i] * input_[(start + i) * channels_ + c]));
                overlap_[o] = hann_[i + hop_] * input_[(start + hop_ + i) * channels_ + c];
            }
        }
        first_ = false;
        produced_ += hop_;
        previous_ = start;
        nominal_ += hop_ * speed_;
        dropConsumed();
    }
}

void TimeStretch::dropConsumed()
{
    // Si tiene solo quello che può ancora servire (dal più piccolo dei due punti, meno il margine della ricerca).
    const auto nominal = static_cast<std::size_t>(std::floor(nominal_));
    std::size_t keep = std::min(previous_, nominal > search_ ? nominal - search_ : 0);
    keep = std::min(keep, inputFrames());
    if (keep < 4096)
        return;  // si sposta la memoria solo ogni tanto
    input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(keep * channels_));
    previous_ -= keep;
    dropped_ += keep;
    nominal_ -= static_cast<double>(keep);
}

void TimeStretch::flush(std::vector<std::int16_t>& out)
{
    if (speed_ == 1.0) {
        pull(out);
        return;
    }
    // Silenzio in fondo, quanto basta per far uscire tutto l'ingresso, poi la coda dell'ultimo pezzo.
    const std::size_t endAbsolute = inputFrames() + dropped_;
    const std::size_t appendedFrom = out.size();
    if (endAbsolute > 0) {
        input_.resize(input_.size() + (window_ * 2 + search_ * 2 + 2) * channels_, 0.0f);
        do {
            const std::size_t before = out.size();
            pull(out);
            if (out.size() == before)
                break;
        } while (static_cast<std::size_t>(nominal_) + dropped_ < endAbsolute);
        for (float v : overlap_)
            out.push_back(toSample(v));
        produced_ += hop_;
        // Il silenzio aggiunto in fondo non fa parte dell'audio: l'uscita dura esattamente ingresso / velocità.
        const auto wanted = static_cast<std::size_t>(std::llround(endAbsolute / speed_));
        if (produced_ > wanted) {
            const std::size_t extra = std::min((produced_ - wanted) * channels_, out.size() - appendedFrom);
            out.resize(out.size() - extra);
        }
    }
    input_.clear();
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    nominal_ = 0.0;
    previous_ = 0;
    dropped_ = 0;
    produced_ = 0;
    first_ = true;
}

}  // namespace editor::core
