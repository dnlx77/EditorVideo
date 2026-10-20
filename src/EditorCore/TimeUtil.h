#pragma once

// Tempi (secondi): intervalli e funzioni di servizio comuni a segmento, progetto, sequenza ed esportazione.

#include <algorithm>
#include <iterator>
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
