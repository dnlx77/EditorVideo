#include "EditorCore/Spectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace editor::core {

namespace {

// FFT radix-2 sul posto (la lunghezza è una potenza di due).
void fft(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

}  // namespace

std::vector<float> spectrumDb(const std::vector<float>& samples)
{
    if (samples.size() != kSpectrumSize)
        return {};
    const std::size_t n = kSpectrumSize;
    std::vector<std::complex<double>> a(n);
    double windowSum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n));
        windowSum += w;
        a[i] = std::complex<double>(static_cast<double>(samples[i]) * w, 0.0);
    }
    fft(a);
    std::vector<float> db(n / 2);
    const double scale = 2.0 / windowSum;  // una sinusoide di ampiezza 1 dà 1 (0 dB)
    for (std::size_t i = 0; i < n / 2; ++i) {
        const double magnitude = std::abs(a[i]) * scale;
        db[i] = static_cast<float>(std::max(-120.0, 20.0 * std::log10(std::max(magnitude, 1e-9))));
    }
    return db;
}

}  // namespace editor::core
