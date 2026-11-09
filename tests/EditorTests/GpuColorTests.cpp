#include "EditorCore/Compositor.h"
#include "EditorCore/GpuColor.h"
#include "EditorCore/GpuDevice.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace editor::core;

namespace {

// Con `allowSoftware`: in CI non c'è una GPU vera e gli stessi shader girano sul rasterizzatore software (WARP).
std::unique_ptr<GpuDevice> makeDevice()
{
    std::string error;
    std::unique_ptr<GpuDevice> device = GpuDevice::create(true, &error);
    if (device)
        std::cout << "[gpu] " << device->info().adapterName << (device->info().software ? " (software)" : "") << ", feature level "
                  << device->info().featureLevel << std::endl;
    else
        std::cout << "[gpu] nessun dispositivo: " << error << std::endl;
    return device;
}

struct FrameDeleter
{
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

// Un fotogramma NV12 con sfumature lisce, dentro la gamma RGB anche con la gamma limitata (così l'andata e ritorno non taglia nulla).
Frame smoothNv12(int w, int h)
{
    Frame frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_NV12;
    frame->width = w;
    frame->height = h;
    EXPECT_GE(av_frame_get_buffer(frame.get(), 32), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            frame->data[0][y * frame->linesize[0] + x] = static_cast<std::uint8_t>(60 + (x * 80) / w + (y * 50) / h);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            frame->data[1][y * frame->linesize[1] + 2 * x] = static_cast<std::uint8_t>(108 + (x * 40) / (w / 2));      // Cb
            frame->data[1][y * frame->linesize[1] + 2 * x + 1] = static_cast<std::uint8_t>(148 - (y * 40) / (h / 2));  // Cr
        }
    return frame;
}

float halfToFloat(std::uint16_t half)
{
    const int sign = (half >> 15) & 1;
    const int exponent = (half >> 10) & 0x1f;
    const int fraction = half & 0x3ff;
    float value;
    if (exponent == 0)
        value = std::ldexp(static_cast<float>(fraction), -24);
    else if (exponent == 31)
        value = fraction == 0 ? INFINITY : NAN;
    else
        value = std::ldexp(static_cast<float>(fraction | 0x400), exponent - 25);
    return sign ? -value : value;
}

}  // namespace

TEST(GpuColor, OddSizesAreRefused)
{
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessun dispositivo D3D11 in questo ambiente";
    EXPECT_EQ(GpuFrameConverter::create(*device, 63, 48), nullptr);
    EXPECT_EQ(GpuFrameConverter::create(*device, 64, 47), nullptr);
    EXPECT_EQ(GpuFrameConverter::create(*device, 0, 0), nullptr);
    EXPECT_NE(GpuFrameConverter::create(*device, 64, 48), nullptr);
}

TEST(GpuColor, SoftwareRasterizerIsRefusedInNormalUse)
{
    // Senza `allowSoftware` si usa solo una scheda vera: altrimenti nullptr (e si compone in CPU).
    std::string error;
    const auto device = GpuDevice::create(false, &error);
    if (device)
        EXPECT_FALSE(device->info().software);
    else
        EXPECT_FALSE(error.empty());
}

class GpuColorRoundTrip : public ::testing::TestWithParam<int>
{
};

TEST_P(GpuColorRoundTrip, Nv12ToRgbaHalfAndBackIsAlmostTheSame)
{
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessun dispositivo D3D11 in questo ambiente";
    const int variant = GetParam();  // 0 = 601 limitata, 1 = 709 limitata, 2 = 601 piena, 3 = 709 piena
    const GpuColorSpace colors = variant % 2 == 0 ? GpuColorSpace::bt601(variant >= 2) : GpuColorSpace::bt709(variant >= 2);
    constexpr int w = 96;
    constexpr int h = 64;
    auto converter = GpuFrameConverter::create(*device, w, h);
    ASSERT_NE(converter, nullptr);
    const Frame input = smoothNv12(w, h);
    ASSERT_TRUE(converter->uploadNv12(input.get(), colors));

    Frame output(av_frame_alloc());
    output->format = AV_PIX_FMT_NV12;
    output->width = w;
    output->height = h;
    ASSERT_GE(av_frame_get_buffer(output.get(), 32), 0);
    ASSERT_TRUE(converter->downloadNv12(output.get(), colors));

    int maxY = 0;
    int maxC = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            maxY = std::max(maxY, std::abs(input->data[0][y * input->linesize[0] + x] - output->data[0][y * output->linesize[0] + x]));
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x)
            maxC = std::max(maxC, std::abs(input->data[1][y * input->linesize[1] + x] - output->data[1][y * output->linesize[1] + x]));
    EXPECT_LE(maxY, 3) << "luminanza, variante " << variant;
    EXPECT_LE(maxC, 4) << "colore, variante " << variant;
}

INSTANTIATE_TEST_SUITE_P(Variants, GpuColorRoundTrip, ::testing::Values(0, 1, 2, 3));

TEST(GpuColor, RgbaHalfMatchesTheSwscaleConversionOfTheCpuPath)
{
    // La composizione CPU converte con swscale e i coefficienti SWS_CS_DEFAULT (BT.601), gamma limitata -> RGB piena.
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessun dispositivo D3D11 in questo ambiente";
    constexpr int w = 96;
    constexpr int h = 64;
    auto converter = GpuFrameConverter::create(*device, w, h);
    ASSERT_NE(converter, nullptr);
    const Frame input = smoothNv12(w, h);
    ASSERT_TRUE(converter->uploadNv12(input.get(), GpuColorSpace::bt601(false)));
    std::vector<std::uint16_t> half;
    ASSERT_TRUE(converter->readRgbaHalf(half));
    ASSERT_EQ(half.size(), static_cast<std::size_t>(w) * h * 4);

    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> sws(
        sws_getContext(w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr), &sws_freeContext);
    ASSERT_NE(sws, nullptr);
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(sws.get(), coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    std::vector<std::uint8_t> cpu(static_cast<std::size_t>(w) * h * 4);
    std::uint8_t* dst[4] = {cpu.data(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws.get(), input->data, input->linesize, 0, h, dst, dstStride);

    double total = 0.0;
    int worst = 0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        if (i % 4 == 3) {
            EXPECT_NEAR(halfToFloat(half[i]), 1.0f, 0.001f);  // alfa opaco
            continue;
        }
        const int gpu = static_cast<int>(std::lround(std::clamp(halfToFloat(half[i]), 0.0f, 1.0f) * 255.0f));
        const int diff = std::abs(gpu - cpu[i]);
        total += diff;
        worst = std::max(worst, diff);
    }
    const double mean = total / (static_cast<double>(w) * h * 3);
    std::cout << "[gpu] differenza dalla CPU: media " << mean << ", massima " << worst << std::endl;
    EXPECT_LT(mean, 2.0);
    EXPECT_LE(worst, 8);
}

// --- Livelli trasformati: stessi risultati di compositeLayer (CPU) ---

namespace {

// Un'immagine BGRA opaca con una sfumatura e dei riquadri (per vedere dove finiscono ritaglio, specchio e rotazione).
std::vector<std::uint8_t> patternBgra(int w, int h, int seed)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t* px = &out[(static_cast<std::size_t>(y) * w + x) * 4];
            const bool square = ((x / 12) + (y / 12) + seed) % 2 == 0;
            px[0] = static_cast<std::uint8_t>(square ? 40 + (x * 150) / w : 200 - (y * 120) / h);  // B
            px[1] = static_cast<std::uint8_t>(60 + (y * 160) / h);                                   // G
            px[2] = static_cast<std::uint8_t>(square ? 220 - (x * 120) / w : 50 + (x * 100) / w);   // R
            px[3] = 255;
        }
    return out;
}

struct LayerCase
{
    const char* name;
    Transform transform;
    int sourceWidth;
    int sourceHeight;
    double maxMean;
};

class GpuLayerCompare : public ::testing::TestWithParam<int>
{
};

}  // namespace

TEST_P(GpuLayerCompare, MatchesCompositeLayer)
{
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessun dispositivo D3D11 in questo ambiente";
    std::vector<LayerCase> cases;
    cases.push_back({"identica", Transform{}, 160, 90, 1.0});
    {
        Transform t;
        t.scaleX = t.scaleY = 0.5;
        t.posX = 20;
        t.posY = -10;
        cases.push_back({"scala 50% e posizione", t, 160, 90, 1.5});
    }
    {
        Transform t;
        t.rotation = 30;
        t.opacity = 0.7;
        cases.push_back({"rotazione 30 e opacita 70%", t, 160, 90, 2.0});
    }
    {
        Transform t;
        t.cropLeft = 0.2;
        t.cropTop = 0.1;
        t.cropRight = 0.15;
        t.flipHorizontal = true;
        cases.push_back({"ritaglio e specchio", t, 160, 90, 1.5});
    }
    {
        Transform t;
        t.scaleX = t.scaleY = 0.2;
        t.posX = -30;
        cases.push_back({"rimpicciolito al 20%", t, 320, 180, 5.0});
    }
    {
        Transform t;
        t.scaleX = 1.8;
        t.scaleY = 1.2;
        t.rotation = -12;
        t.posY = 8;
        cases.push_back({"ingrandito e ruotato", t, 100, 100, 3.0});
    }
    const LayerCase& c = cases[static_cast<std::size_t>(GetParam())];

    constexpr int w = 128;
    constexpr int h = 72;
    auto converter = GpuFrameConverter::create(*device, w, h);
    ASSERT_NE(converter, nullptr);
    std::vector<std::uint8_t> canvas = patternBgra(w, h, 1);
    const std::vector<std::uint8_t> sourcePixels = patternBgra(c.sourceWidth, c.sourceHeight, 0);
    const BgraImage source{sourcePixels.data(), c.sourceWidth * 4, c.sourceWidth, c.sourceHeight};

    ASSERT_TRUE(converter->setCanvasBgra({canvas.data(), w * 4, w, h}));
    ASSERT_TRUE(converter->drawLayer(source, c.transform));
    std::vector<std::uint16_t> half;
    ASSERT_TRUE(converter->readRgbaHalf(half));

    compositeLayer(canvas.data(), w * 4, w, h, source, c.transform);

    double total = 0.0;
    int worst = 0;
    for (int i = 0; i < w * h; ++i)
        for (int ch = 0; ch < 3; ++ch) {
            const float value = halfToFloat(half[static_cast<std::size_t>(i) * 4 + (ch == 0 ? 2 : ch == 2 ? 0 : 1)]);  // BGRA <- RGBA
            const int gpu = static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            const int diff = std::abs(gpu - canvas[static_cast<std::size_t>(i) * 4 + ch]);
            total += diff;
            worst = std::max(worst, diff);
        }
    const double mean = total / (static_cast<double>(w) * h * 3);
    std::cout << "[gpu] livello «" << c.name << "»: differenza dalla CPU media " << mean << ", massima " << worst << std::endl;
    EXPECT_LT(mean, c.maxMean) << c.name;
}

INSTANTIATE_TEST_SUITE_P(Cases, GpuLayerCompare, ::testing::Range(0, 6));
