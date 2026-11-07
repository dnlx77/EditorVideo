#include "EditorCore/AudioEffect.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace editor::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Coeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

// Ricette di Robert Bristow-Johnson (Audio EQ Cookbook), coefficienti già divisi per a0.
Coeffs design(const EqBand& band, int sampleRate)
{
    const double freq = std::clamp(band.freq, kEqMinHz, std::min(kEqMaxHz, sampleRate * 0.45));
    const double q = std::clamp(band.q, kEqMinQ, kEqMaxQ);
    const double w0 = 2.0 * kPi * freq / sampleRate;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double alpha = sinw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    const double A = std::pow(10.0, band.gainDb / 40.0);
    switch (band.type) {
    case EqBandType::LowCut:
        b0 = (1 + cosw) / 2;
        b1 = -(1 + cosw);
        b2 = (1 + cosw) / 2;
        a0 = 1 + alpha;
        a1 = -2 * cosw;
        a2 = 1 - alpha;
        break;
    case EqBandType::HighCut:
        b0 = (1 - cosw) / 2;
        b1 = 1 - cosw;
        b2 = (1 - cosw) / 2;
        a0 = 1 + alpha;
        a1 = -2 * cosw;
        a2 = 1 - alpha;
        break;
    case EqBandType::Bell:
        b0 = 1 + alpha * A;
        b1 = -2 * cosw;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cosw;
        a2 = 1 - alpha / A;
        break;
    case EqBandType::LowShelf: {
        const double sq = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cosw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cosw);
        b2 = A * ((A + 1) - (A - 1) * cosw - sq);
        a0 = (A + 1) + (A - 1) * cosw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cosw);
        a2 = (A + 1) + (A - 1) * cosw - sq;
        break;
    }
    case EqBandType::HighShelf: {
        const double sq = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cosw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cosw);
        b2 = A * ((A + 1) + (A - 1) * cosw - sq);
        a0 = (A + 1) - (A - 1) * cosw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cosw);
        a2 = (A + 1) - (A - 1) * cosw - sq;
        break;
    }
    default:
        return {};
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

}  // namespace

bool EqBand::active() const
{
    if (!enabled)
        return false;
    return hasGain() ? std::abs(gainDb) >= 0.05 : true;
}

std::vector<EqBand> defaultEqBands()
{
    return {
        {EqBandType::LowCut, false, 80.0, 0.0, 0.71},      {EqBandType::LowShelf, true, 200.0, 0.0, 0.71},
        {EqBandType::Bell, true, 500.0, 0.0, 0.71},        {EqBandType::Bell, true, 3000.0, 0.0, 0.71},
        {EqBandType::HighShelf, true, 8000.0, 0.0, 0.71},  {EqBandType::HighCut, false, 18000.0, 0.0, 0.71},
    };
}

EqBand clampedBand(EqBand band)
{
    const auto finite = [](double v, double fallback) { return std::isfinite(v) ? v : fallback; };
    band.freq = std::clamp(finite(band.freq, 1000.0), kEqMinHz, kEqMaxHz);
    band.q = std::clamp(finite(band.q, 0.71), kEqMinQ, kEqMaxQ);
    band.gainDb = band.hasGain() ? std::clamp(finite(band.gainDb, 0.0), -kEqMaxGainDb, kEqMaxGainDb) : 0.0;
    return band;
}

bool AudioEffect::active() const
{
    if (!enabled)
        return false;
    switch (kind) {
    case EffectKind::Equalizer:
        return std::any_of(bands.begin(), bands.end(), [](const EqBand& b) { return b.active(); });
    case EffectKind::Normalize:
        return true;
    }
    return false;
}

bool hasActiveNormalize(const std::vector<AudioEffect>& effects)
{
    return std::any_of(effects.begin(), effects.end(), [](const AudioEffect& e) { return e.kind == EffectKind::Normalize && e.enabled; });
}

float normalizeGain(const std::vector<AudioEffect>& effects, float measuredPeak)
{
    for (const AudioEffect& e : effects) {
        if (e.kind != EffectKind::Normalize || !e.enabled)
            continue;
        if (!(measuredPeak > 1e-5f))
            return 1.0f;
        const double target = std::pow(10.0, clampedEffect(e).targetDb / 20.0);
        return static_cast<float>(std::min(target / static_cast<double>(measuredPeak), 100.0));
    }
    return 1.0f;
}

const char* effectName(EffectKind kind)
{
    switch (kind) {
    case EffectKind::Equalizer:
        return "Equalizzatore";
    case EffectKind::Normalize:
        return "Normalizza";
    }
    return "";
}

const char* bandTypeName(EqBandType type)
{
    switch (type) {
    case EqBandType::LowCut:
        return "Taglio bassi";
    case EqBandType::LowShelf:
        return "Shelf bassi";
    case EqBandType::Bell:
        return "Campana";
    case EqBandType::HighShelf:
        return "Shelf alti";
    case EqBandType::HighCut:
        return "Taglio alti";
    }
    return "";
}

AudioEffect clampedEffect(AudioEffect effect)
{
    switch (effect.kind) {
    case EffectKind::Equalizer:
        if (effect.bands.empty())
            effect.bands = defaultEqBands();
        if (effect.bands.size() > 8)
            effect.bands.resize(8);
        for (EqBand& b : effect.bands)
            b = clampedBand(b);
        break;
    case EffectKind::Normalize:
        effect.targetDb = std::clamp(std::isfinite(effect.targetDb) ? effect.targetDb : -1.0, kNormalizeMinDb, kNormalizeMaxDb);
        break;
    }
    return effect;
}

double eqResponseDb(const std::vector<EqBand>& bands, double hz, int sampleRate)
{
    const double w = 2.0 * kPi * hz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    double db = 0.0;
    for (const EqBand& band : bands) {
        if (!band.active())
            continue;
        const Coeffs c = design(band, sampleRate);
        const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
        db += 20.0 * std::log10(std::max(1e-9, std::abs(num / den)));
    }
    return db;
}

void EqualizerFilter::setBands(const std::vector<EqBand>& bands, int sampleRate)
{
    stages_.resize(bands.size());
    for (std::size_t i = 0; i < bands.size(); ++i) {
        Biquad& q = stages_[i];
        const bool wasOn = q.on;
        q.on = bands[i].active();
        if (!q.on) {
            q.b0 = 1;
            q.b1 = q.b2 = q.a1 = q.a2 = 0;
            continue;
        }
        if (!wasOn) {  // si riaccende: parte da uno stato pulito
            for (int c = 0; c < 2; ++c)
                q.x1[c] = q.x2[c] = q.y1[c] = q.y2[c] = 0.0;
        }
        const Coeffs c = design(bands[i], sampleRate);
        q.b0 = c.b0;
        q.b1 = c.b1;
        q.b2 = c.b2;
        q.a1 = c.a1;
        q.a2 = c.a2;
    }
}

void EqualizerFilter::reset()
{
    for (Biquad& q : stages_) {
        for (int c = 0; c < 2; ++c)
            q.x1[c] = q.x2[c] = q.y1[c] = q.y2[c] = 0.0;
    }
}

void EqualizerFilter::process(float* samples, std::size_t frames)
{
    for (Biquad& q : stages_) {
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
