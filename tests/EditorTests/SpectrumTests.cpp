#include "EditorCore/Spectrum.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace editor::core;

TEST(SpectrumTest, SineShowsPeakAtItsFrequency)
{
    std::vector<float> samples(kSpectrumSize);
    const double hz = 1000.0;
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / 48000.0));
    const std::vector<float> db = spectrumDb(samples);
    ASSERT_EQ(db.size(), kSpectrumSize / 2);
    const auto peak = std::max_element(db.begin(), db.end());
    const double peakHz = static_cast<double>(peak - db.begin()) * 48000.0 / static_cast<double>(kSpectrumSize);
    EXPECT_NEAR(peakHz, hz, 48000.0 / kSpectrumSize);
    EXPECT_NEAR(*peak, 0.0, 1.5);  // una sinusoide a fondo scala sta vicino a 0 dB
    EXPECT_LT(db[db.size() - 1], -60.0f);
}

TEST(SpectrumTest, SilenceIsFloorAndWrongSizeIsEmpty)
{
    const std::vector<float> db = spectrumDb(std::vector<float>(kSpectrumSize, 0.0f));
    EXPECT_FLOAT_EQ(db[10], -120.0f);
    EXPECT_TRUE(spectrumDb(std::vector<float>(100)).empty());
}
