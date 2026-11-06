#pragma once

#include <array>
#include <vector>

namespace editor::core {

// Un effetto messo su una traccia audio (o sul Master) nel Mixer.
enum class EffectKind
{
    Equalizer,  // 3 bande: bassi (shelf a 120 Hz), medi (campana a 1 kHz), alti (shelf a 6 kHz); params = guadagni in dB
};

inline constexpr double kEffectMinDb = -12.0;
inline constexpr double kEffectMaxDb = 12.0;

struct AudioEffect
{
    EffectKind kind = EffectKind::Equalizer;
    bool enabled = true;  // false = escluso (resta nello slot ma non agisce)
    std::array<double, 4> params{};

    // Se agisce davvero sull'audio: attivo e con almeno un parametro che cambia qualcosa.
    bool active() const;
    bool operator==(const AudioEffect&) const = default;
};

const char* effectName(EffectKind kind);  // "Equalizzatore"
// Porta i parametri nei limiti (e quelli inutilizzati a 0).
AudioEffect clampedEffect(AudioEffect effect);

// Equalizzatore a 3 bande (filtri biquad RBJ in forma diretta I), sinistra e destra separati. I coefficienti si
// ricalcolano ai cambi di parametri senza toccare lo stato (niente scatti); reset() azzera lo stato (dopo un salto).
class EqualizerFilter
{
public:
    void setGains(double lowDb, double midDb, double highDb, int sampleRate);
    void reset();
    // Filtra `frames` fotogrammi stereo interlacciati (float) sul posto.
    void process(float* samples, std::size_t frames);

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        bool on = false;
        double x1[2] = {0, 0}, x2[2] = {0, 0}, y1[2] = {0, 0}, y2[2] = {0, 0};
    };
    std::array<Biquad, 3> bands_;
};

}  // namespace editor::core
