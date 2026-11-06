#include "EditorCore/AudioEffect.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

namespace {

constexpr double kLowHz = 120.0;
constexpr double kMidHz = 1000.0;
constexpr double kHighHz = 6000.0;
constexpr double kPi = 3.14159265358979323846;

}  // namespace

bool AudioEffect::active() const
{
    if (!enabled)
        return false;
    switch (kind) {
    case EffectKind::Equalizer:
        return std::abs(params[0]) >= 0.05 || std::abs(params[1]) >= 0.05 || std::abs(params[2]) >= 0.05;
    }
    return false;
}

const char* effectName(EffectKind kind)
{
    switch (kind) {
    case EffectKind::Equalizer:
        return "Equalizzatore";
    }
    return "";
}

AudioEffect clampedEffect(AudioEffect effect)
{
    switch (effect.kind) {
    case EffectKind::Equalizer:
        for (std::size_t i = 0; i < 3; ++i)
            effect.params[i] = std::isfinite(effect.params[i]) ? std::clamp(effect.params[i], kEffectMinDb, kEffectMaxDb) : 0.0;
        effect.params[3] = 0.0;
        break;
    }
    return effect;
}

void EqualizerFilter::setGains(double lowDb, double midDb, double highDb, int sampleRate)
{
    const double gains[3] = {lowDb, midDb, highDb};
    const double freqs[3] = {kLowHz, kMidHz, kHighHz};
    for (int i = 0; i < 3; ++i) {
        Biquad& q = bands_[static_cast<std::size_t>(i)];
        const bool wasOn = q.on;
        q.on = std::abs(gains[i]) >= 0.05;
        if (q.on && !wasOn) {  // si riaccende: parte da uno stato pulito
            for (int c = 0; c < 2; ++c)
                q.x1[c] = q.x2[c] = q.y1[c] = q.y2[c] = 0.0;
        }
        if (!q.on) {
            q.b0 = 1;
            q.b1 = q.b2 = q.a1 = q.a2 = 0;
            continue;
        }
        // Ricette di Robert Bristow-Johnson (Audio EQ Cookbook).
        const double A = std::pow(10.0, gains[i] / 40.0);
        const double w0 = 2.0 * kPi * freqs[i] / sampleRate;
        const double cosw = std::cos(w0);
        const double sinw = std::sin(w0);
        double b0, b1, b2, a0, a1, a2;
        if (i == 1) {  // campana, Q = 0,9
            const double alpha = sinw / (2.0 * 0.9);
            b0 = 1 + alpha * A;
            b1 = -2 * cosw;
            b2 = 1 - alpha * A;
            a0 = 1 + alpha / A;
            a1 = -2 * cosw;
            a2 = 1 - alpha / A;
        } else {  // shelf con pendenza S = 1
            const double alpha = sinw / 2.0 * std::sqrt(2.0);
            const double sq = 2.0 * std::sqrt(A) * alpha;
            if (i == 0) {
                b0 = A * ((A + 1) - (A - 1) * cosw + sq);
                b1 = 2 * A * ((A - 1) - (A + 1) * cosw);
                b2 = A * ((A + 1) - (A - 1) * cosw - sq);
                a0 = (A + 1) + (A - 1) * cosw + sq;
                a1 = -2 * ((A - 1) + (A + 1) * cosw);
                a2 = (A + 1) + (A - 1) * cosw - sq;
            } else {
                b0 = A * ((A + 1) + (A - 1) * cosw + sq);
                b1 = -2 * A * ((A - 1) + (A + 1) * cosw);
                b2 = A * ((A + 1) + (A - 1) * cosw - sq);
                a0 = (A + 1) - (A - 1) * cosw + sq;
                a1 = 2 * ((A - 1) - (A + 1) * cosw);
                a2 = (A + 1) - (A - 1) * cosw - sq;
            }
        }
        q.b0 = b0 / a0;
        q.b1 = b1 / a0;
        q.b2 = b2 / a0;
        q.a1 = a1 / a0;
        q.a2 = a2 / a0;
    }
}

void EqualizerFilter::reset()
{
    for (Biquad& q : bands_) {
        for (int c = 0; c < 2; ++c)
            q.x1[c] = q.x2[c] = q.y1[c] = q.y2[c] = 0.0;
    }
}

void EqualizerFilter::process(float* samples, std::size_t frames)
{
    for (Biquad& q : bands_) {
        if (!q.on)
            continue;
        for (std::size_t i = 0; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                const double x = samples[i * 2 + static_cast<std::size_t>(c)];
                const double y = q.b0 * x + q.b1 * q.x1[c] + q.b2 * q.x2[c] - q.a1 * q.y1[c] - q.a2 * q.y2[c];
                q.x2[c] = q.x1[c];
                q.x1[c] = x;
                q.y2[c] = q.y1[c];
                q.y1[c] = y;
                samples[i * 2 + static_cast<std::size_t>(c)] = static_cast<float>(y);
            }
        }
    }
}

}  // namespace editor::core
