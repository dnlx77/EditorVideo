#include "EditorCore/AudioEffect.h"
#include "EditorCore/AudioTracks.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace editor::core;

namespace {

// Ampiezza di uscita (a regime) di un seno a `hz` passato dal filtro con i guadagni dati, rispetto all'ingresso.
double gainAt(double hz, double lowDb, double midDb, double highDb)
{
    constexpr int kRate = 48000;
    constexpr double kPi = 3.14159265358979323846;
    EqualizerFilter filter;
    filter.setGains(lowDb, midDb, highDb, kRate);
    std::vector<float> samples(kRate * 2);
    for (int i = 0; i < kRate; ++i) {
        const float v = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / kRate));
        samples[static_cast<std::size_t>(i) * 2] = v;
        samples[static_cast<std::size_t>(i) * 2 + 1] = v;
    }
    filter.process(samples.data(), kRate);
    float peak = 0.0f;
    for (int i = kRate / 2; i < kRate; ++i)  // a regime: la seconda metà
        peak = std::max(peak, std::abs(samples[static_cast<std::size_t>(i) * 2]));
    return peak / 0.25;
}

}  // namespace

TEST(AudioEffect, FlatEqualizerChangesNothing)
{
    EXPECT_NEAR(gainAt(1000.0, 0, 0, 0), 1.0, 1e-3);
    AudioEffect flat;
    EXPECT_FALSE(flat.active());
    flat.params[1] = 3.0;
    EXPECT_TRUE(flat.active());
    flat.enabled = false;
    EXPECT_FALSE(flat.active()) << "escluso non agisce";
}

TEST(AudioEffect, EachBandMovesItsOwnRegion)
{
    EXPECT_NEAR(gainAt(40.0, 12, 0, 0), std::pow(10.0, 12.0 / 20.0), 0.35) << "bassi";
    EXPECT_NEAR(gainAt(1000.0, 0, 6, 0), std::pow(10.0, 6.0 / 20.0), 0.05) << "medi";
    EXPECT_NEAR(gainAt(15000.0, 0, 0, -12), std::pow(10.0, -12.0 / 20.0), 0.05) << "alti";
    EXPECT_NEAR(gainAt(1000.0, 12, 0, 12), 1.0, 0.12) << "le bande laterali non toccano i medi";
    EXPECT_LT(gainAt(1000.0, 0, -12, 0), 0.3);
}

TEST(AudioEffect, ParametersAreClamped)
{
    AudioEffect e;
    e.params = {99.0, -99.0, std::nan(""), 7.0};
    e = clampedEffect(e);
    EXPECT_EQ(e.params[0], kEffectMaxDb);
    EXPECT_EQ(e.params[1], kEffectMinDb);
    EXPECT_EQ(e.params[2], 0.0);
    EXPECT_EQ(e.params[3], 0.0);
}

TEST(AudioTracks, EffectsAreKeptPerTrackAndOnTheMaster)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    AudioEffect eq;
    eq.params = {3.0, 0.0, -2.0, 0.0};
    EXPECT_TRUE(audio.setEffects(a2, {eq}));
    EXPECT_FALSE(audio.setEffects(a2, {eq}));
    EXPECT_EQ(audio.effects(a2).size(), 1u);
    EXPECT_TRUE(audio.effects(AudioTracks::kMasterTrack).empty());
    EXPECT_FALSE(audio.setEffects(99, {eq}));
    EXPECT_TRUE(audio.setEffects(AudioTracks::kMasterTrack, {eq}));
    EXPECT_FALSE(audio.isDefault());
    EXPECT_TRUE(audio.needsMix()) << "un effetto attivo sul Master cambia l'audio";
    // Escluso, A1 non ha bisogno del mix.
    AudioTracks other;
    eq.enabled = false;
    EXPECT_TRUE(other.setEffects(AudioTracks::kOriginalTrack, {eq}));
    EXPECT_FALSE(other.needsMix());
    eq.enabled = true;
    EXPECT_TRUE(other.setEffects(AudioTracks::kOriginalTrack, {eq}));
    EXPECT_TRUE(other.needsMix());
    audio.clear();
    EXPECT_TRUE(audio.effects(AudioTracks::kMasterTrack).empty());
}
