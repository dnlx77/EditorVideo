#pragma once

#include <cstddef>
#include <vector>

namespace editor::core {

// Quanti campioni (mono) servono per un'analisi dello spettro.
inline constexpr std::size_t kSpectrumSize = 4096;

// Lo spettro di ampiezza di `samples` (kSpectrumSize campioni mono, finestra di Hann, FFT): kSpectrumSize / 2 valori in
// dB (0 dB = una sinusoide a fondo scala, -120 come minimo), dal bin 0 (0 Hz) in su, ognuno largo sampleRate / kSpectrumSize.
// Con un numero di campioni diverso da kSpectrumSize dà un vettore vuoto.
std::vector<float> spectrumDb(const std::vector<float>& samples);

}  // namespace editor::core
