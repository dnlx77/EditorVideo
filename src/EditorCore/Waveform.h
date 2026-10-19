#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace editor::core {

// Forma d'onda di un file, per disegnarla: il picco dell'audio (tutti i canali) ogni `binSeconds`.
struct Waveform
{
    double binSeconds = 0.01;
    std::vector<std::uint8_t> peaks;  // 0 = silenzio, 255 = volume massimo

    bool empty() const { return peaks.empty(); }
    double duration() const { return peaks.size() * binSeconds; }
    // Picco più alto in [from, to) secondi del file, da 0 a 1.
    double peak(double from, double to) const;
};

// Decodifica tutto l'audio principale del file (percorso UTF-8). Forma d'onda vuota se il file non ha audio o se
// `cancel` diventa true. Va chiamata fuori dal thread dell'interfaccia: per un file lungo ci vuole qualche secondo.
Waveform computeWaveform(const std::string& utf8Path, const std::atomic<bool>* cancel = nullptr);

}  // namespace editor::core
