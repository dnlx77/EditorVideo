#include "EditorCore/AudioEffect.h"
#include "EditorCore/AudioTracks.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace editor::core;

namespace {

constexpr int kRate = 48000;

// Le sei bande di partenza con un cambio a una sola (indice 0-based).
std::vector<EqBand> bandsWith(std::size_t index, EqBand band)
{
    std::vector<EqBand> bands = defaultEqBands();
    bands[index] = band;
    return bands;
}

// Ampiezza di uscita (a regime) di un seno a `hz` passato dal filtro con le bande date, rispetto all'ingresso.
double gainAt(double hz, const std::vector<EqBand>& bands)
{
    constexpr double kPi = 3.14159265358979323846;
    EqualizerFilter filter;
    filter.setBands(bands, kRate);
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

double db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

TEST(AudioEffect, DefaultEqualizerChangesNothing)
{
    const std::vector<EqBand> bands = defaultEqBands();
    ASSERT_EQ(bands.size(), kEqBandCount);
    EXPECT_NEAR(gainAt(1000.0, bands), 1.0, 1e-3);
    AudioEffect eq;
    EXPECT_FALSE(eq.active()) << "senza guadagni e con i tagli spenti non agisce";
    eq.bands[2].gainDb = 3.0;
    EXPECT_TRUE(eq.active());
    eq.enabled = false;
    EXPECT_FALSE(eq.active()) << "escluso non agisce";
    EXPECT_FALSE(bands[0].enabled);
    EXPECT_FALSE(bands[5].enabled);
}

TEST(AudioEffect, EachBandTypeActsWhereItShould)
{
    EXPECT_NEAR(db(gainAt(1000.0, bandsWith(2, {EqBandType::Bell, true, 1000.0, 6.0, 0.71}))), 6.0, 0.3) << "campana";
    EXPECT_LT(db(gainAt(40.0, bandsWith(1, {EqBandType::LowShelf, true, 200.0, -12.0, 0.71}))), -9.0) << "shelf bassi";
    EXPECT_NEAR(db(gainAt(5000.0, bandsWith(1, {EqBandType::LowShelf, true, 200.0, -12.0, 0.71}))), 0.0, 0.3);
    EXPECT_GT(db(gainAt(15000.0, bandsWith(4, {EqBandType::HighShelf, true, 4000.0, 9.0, 0.71}))), 7.0) << "shelf alti";
    EXPECT_LT(db(gainAt(30.0, bandsWith(0, {EqBandType::LowCut, true, 200.0, 0.0, 0.71}))), -20.0) << "taglio bassi";
    EXPECT_NEAR(db(gainAt(5000.0, bandsWith(0, {EqBandType::LowCut, true, 200.0, 0.0, 0.71}))), 0.0, 0.3);
    EXPECT_LT(db(gainAt(15000.0, bandsWith(5, {EqBandType::HighCut, true, 3000.0, 0.0, 0.71}))), -20.0) << "taglio alti";
    EXPECT_NEAR(db(gainAt(200.0, bandsWith(5, {EqBandType::HighCut, true, 3000.0, 0.0, 0.71}))), 0.0, 0.3);
    // Una banda spenta non fa niente anche con un guadagno.
    EXPECT_NEAR(gainAt(1000.0, bandsWith(2, {EqBandType::Bell, false, 1000.0, 12.0, 0.71})), 1.0, 1e-3);
}

TEST(AudioEffect, ResponseMatchesTheFilter)
{
    const std::vector<EqBand> bands = {{EqBandType::LowCut, true, 100.0, 0.0, 0.71},
                                       {EqBandType::Bell, true, 1000.0, 6.0, 1.5},
                                       {EqBandType::HighShelf, true, 6000.0, -4.0, 0.71}};
    for (const double hz : {50.0, 100.0, 500.0, 1000.0, 3000.0, 10000.0})
        EXPECT_NEAR(eqResponseDb(bands, hz, kRate), db(gainAt(hz, bands)), 0.1) << hz << " Hz";
    EXPECT_NEAR(eqResponseDb(defaultEqBands(), 1000.0, kRate), 0.0, 1e-9);
}

TEST(AudioEffect, ParametersAreClamped)
{
    AudioEffect e;
    e.bands = {{EqBandType::Bell, true, 5.0, 99.0, 99.0}, {EqBandType::LowCut, true, 1e9, 7.0, -1.0},
               {EqBandType::Bell, true, std::nan(""), std::nan(""), std::nan("")}};
    e = clampedEffect(e);
    EXPECT_EQ(e.bands[0].freq, kEqMinHz);
    EXPECT_EQ(e.bands[0].gainDb, kEqMaxGainDb);
    EXPECT_EQ(e.bands[0].q, kEqMaxQ);
    EXPECT_EQ(e.bands[1].freq, kEqMaxHz);
    EXPECT_EQ(e.bands[1].gainDb, 0.0) << "un taglio non ha guadagno";
    EXPECT_EQ(e.bands[1].q, kEqMinQ);
    EXPECT_EQ(e.bands[2].gainDb, 0.0);
    AudioEffect empty;
    empty.bands.clear();
    EXPECT_EQ(clampedEffect(empty).bands.size(), kEqBandCount) << "senza bande: quelle di partenza";
}

TEST(AudioTracks, EffectsAreKeptPerTrackAndOnTheMaster)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    AudioEffect eq;
    eq.bands[1].gainDb = 3.0;
    EXPECT_TRUE(audio.setEffects(a2, {eq}));
    EXPECT_FALSE(audio.setEffects(a2, {eq}));
    EXPECT_EQ(audio.effects(a2).size(), 1u);
    EXPECT_TRUE(audio.effects(AudioTracks::kMasterTrack).empty());
    EXPECT_FALSE(audio.setEffects(99, {eq}));
    EXPECT_TRUE(audio.setEffects(AudioTracks::kMasterTrack, {eq, eq}));
    EXPECT_EQ(audio.effects(AudioTracks::kMasterTrack).size(), 2u) << "più equalizzatori nello stesso slot";
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

TEST(AudioEffectTest, NormalizeGainBringsThePeakToTheTarget)
{
    AudioEffect norm;
    norm.kind = EffectKind::Normalize;
    norm.targetDb = -6.0;
    const float expected = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    EXPECT_NEAR(normalizeGain({norm}, 0.25f), expected / 0.25f, 1e-4);
    EXPECT_NEAR(normalizeGain({norm}, 1.0f), expected, 1e-4) << "un picco troppo alto si abbassa";
    EXPECT_FLOAT_EQ(normalizeGain({norm}, 0.0f), 1.0f) << "mix muto: niente da fare";
    EXPECT_LE(normalizeGain({norm}, 1e-4f), 100.0f) << "mai oltre +40 dB";
    EXPECT_TRUE(norm.active());
    EXPECT_TRUE(hasActiveNormalize({norm}));
    norm.enabled = false;
    EXPECT_FLOAT_EQ(normalizeGain({norm}, 0.25f), 1.0f);
    EXPECT_FALSE(hasActiveNormalize({norm}));
    EXPECT_FALSE(hasActiveNormalize({}));
    norm.enabled = true;
    norm.targetDb = 5.0;
    EXPECT_DOUBLE_EQ(clampedEffect(norm).targetDb, kNormalizeMaxDb);
    norm.targetDb = -80.0;
    EXPECT_DOUBLE_EQ(clampedEffect(norm).targetDb, kNormalizeMinDb);
}
