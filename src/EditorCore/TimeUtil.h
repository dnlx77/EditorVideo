#pragma once

// Tempi (secondi): intervalli e funzioni di servizio comuni a segmento, progetto, sequenza ed esportazione.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

namespace editor::core {

// Intervallo di tempo [start, end) in secondi.
struct TimeRange
{
    double start = 0.0;
    double end = 0.0;

    double length() const { return end - start; }
    bool operator==(const TimeRange&) const = default;
};

// Tolleranza sui confronti fra tempi: i tempi dei fotogrammi arrivano da calcoli in virgola mobile.
inline constexpr double kTimeEpsilon = 0.001;

// Gli stessi tratti in ordine di tempo, senza quelli vuoti (o al contrario) e con quelli che si toccano o si
// sovrappongono uniti in uno solo.
inline std::vector<TimeRange> mergeRanges(std::vector<TimeRange> ranges)
{
    std::erase_if(ranges, [](const TimeRange& r) { return r.end <= r.start + kTimeEpsilon; });
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : ranges) {
        if (!merged.empty() && r.start <= merged.back().end + kTimeEpsilon)
            merged.back().end = std::max(merged.back().end, r.end);
        else
            merged.push_back(r);
    }
    return merged;
}

// Dove mettere un oggetto lungo `length` che si vorrebbe a `desired` (portato a >= 0) senza sovrapporlo agli `occupied`
// (in qualsiasi ordine, anche sovrapposti fra loro): `desired` se è libero, altrimenti il punto più vicino in uno degli
// spazi fra gli occupati (o dopo l'ultimo). Si tollerano sovrapposizioni fino a `tolerance` secondi.
inline double closestFreeStart(std::vector<TimeRange> occupied, double length, double desired, double tolerance)
{
    desired = std::max(desired, 0.0);
    const auto overlaps = [&](double start) {
        return std::any_of(occupied.begin(), occupied.end(), [&](const TimeRange& r) {
            return start < r.end - tolerance && start + length > r.start + tolerance;
        });
    };
    if (!overlaps(desired))
        return desired;
    std::sort(occupied.begin(), occupied.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    double best = -1.0;
    double bestDistance = std::numeric_limits<double>::max();
    const auto consider = [&](double gapStart, double gapEnd) {
        if (gapEnd - gapStart < length - tolerance)
            return;
        const double start = std::clamp(desired, gapStart, std::max(gapStart, gapEnd - length));
        const double distance = std::abs(start - desired);
        if (distance <= bestDistance) {  // a pari distanza vince il posto più avanti
            bestDistance = distance;
            best = start;
        }
    };
    double cursor = 0.0;
    for (const TimeRange& r : occupied) {
        consider(cursor, r.start);
        cursor = std::max(cursor, r.end);
    }
    consider(cursor, std::numeric_limits<double>::max() / 4);
    return best;
}

// Fotogramma chiave nello stesso punto o subito prima di `seconds` (`keyframes` in ordine); se `seconds` viene prima
// del primo fotogramma chiave, il primo. Senza fotogrammi chiave restituisce `seconds`.
inline double snapToKeyframe(const std::vector<double>& keyframes, double seconds)
{
    if (keyframes.empty())
        return seconds;
    // Primo fotogramma chiave strettamente dopo `seconds` (con tolleranza): quello prima è il nostro.
    const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), seconds + kTimeEpsilon);
    return next == keyframes.begin() ? keyframes.front() : *std::prev(next);
}

// `seconds` riportato dentro il file [0, duration]; con durata sconosciuta (<= 0) solo non negativo.
inline double clampTo(double seconds, double duration)
{
    return duration > 0 ? std::clamp(seconds, 0.0, duration) : std::max(0.0, seconds);
}

}  // namespace editor::core
