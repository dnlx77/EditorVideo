#include "EditorCore/TimeStretch.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using editor::core::TimeStretch;

namespace {

constexpr int kRate = 48000;

// `seconds` di una sinusoide a `frequency` Hz, stereo.
std::vector<std::int16_t> tone(double frequency, double seconds)
{
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<std::int16_t> samples(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<std::int16_t>(10000 * std::sin(2 * std::numbers::pi * frequency * i / kRate));
        samples[i * 2] = v;
        samples[i * 2 + 1] = v;
    }
    return samples;
}

// Passa i campioni a blocchi di ~21 ms (come arrivano dal decoder) e restituisce tutta l'uscita.
std::vector<std::int16_t> stretch(const std::vector<std::int16_t>& input, double speed)
{
    TimeStretch stretcher(2, kRate, speed);
    std::vector<std::int16_t> out;
    constexpr std::size_t block = 1024 * 2;
    for (std::size_t at = 0; at < input.size(); at += block) {
        stretcher.push(input.data() + at, std::min(block, input.size() - at));
        stretcher.pull(out);
    }
    stretcher.flush(out);
    return out;
}

// Frequenza stimata dai passaggi per lo zero del canale sinistro, saltando il primo e l'ultimo decimo.
double frequencyOf(const std::vector<std::int16_t>& samples)
{
    const std::size_t frames = samples.size() / 2;
    const std::size_t from = frames / 10;
    const std::size_t to = frames - frames / 10;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if ((samples[(i - 1) * 2] < 0) != (samples[i * 2] < 0))
            ++crossings;
    }
    return crossings / 2.0 / (static_cast<double>(to - from) / kRate);
}

TEST(TimeStretch, NormalSpeedPassesSamplesUnchanged)
{
    const auto input = tone(440, 0.5);
    EXPECT_EQ(stretch(input, 1.0), input);
}

TEST(TimeStretch, KeepsPitchAndChangesLength)
{
    const auto input = tone(440, 2.0);
    for (double speed : {0.25, 0.5, 1.5, 2.0}) {
        SCOPED_TRACE(speed);
        const auto out = stretch(input, speed);
        const double seconds = out.size() / 2.0 / kRate;
        EXPECT_NEAR(seconds, 2.0 / speed, 0.05);  // durata divisa per la velocità
        EXPECT_NEAR(frequencyOf(out), 440.0, 440.0 * 0.03);  // stessa nota
    }
}

TEST(TimeStretch, SilenceStaysSilent)
{
    const std::vector<std::int16_t> input(kRate * 2, 0);
    for (std::int16_t v : stretch(input, 2.0))
        ASSERT_EQ(v, 0);
}

TEST(TimeStretch, NoClicksOnASteadyTone)
{
    // Una sinusoide ricostruita bene non ha salti più grandi di quelli della sinusoide stessa (più un margine).
    const auto out = stretch(tone(220, 1.0), 0.5);
    const double maxStep = 10000 * 2 * std::numbers::pi * 220 / kRate;
    const std::size_t frames = out.size() / 2;
    int clicks = 0;
    for (std::size_t i = frames / 10; i + 1 < frames - frames / 10; ++i) {
        if (std::abs(out[(i + 1) * 2] - out[i * 2]) > maxStep * 2.5)
            ++clicks;
    }
    EXPECT_LE(clicks, 2);
}

}  // namespace
