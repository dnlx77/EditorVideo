#pragma once

// Funzioni di servizio sui tempi (secondi), comuni a segmento, elenco dei tratti, progetto e sequenza.

#include <algorithm>
#include <iterator>
#include <vector>

namespace editor::core {

// Tolleranza sui confronti fra tempi: i tempi dei fotogrammi arrivano da calcoli in virgola mobile.
inline constexpr double kTimeEpsilon = 0.001;

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
