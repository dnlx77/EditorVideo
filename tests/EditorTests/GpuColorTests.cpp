#include "EditorCore/Compositor.h"
#include "EditorCore/GpuColor.h"
#include "EditorCore/GpuCompositor.h"
#include "EditorCore/TextFrame.h"
#include "EditorCore/GpuDevice.h"
#include "EditorCore/TextImage.h"
#include "EditorCore/Transitions.h"
#include "EditorCore/TransitionRender.h"

#ifdef _WIN32
#include "EditorCore/GpuInternal.h"
#include <d3d11.h>  // fuori da extern "C" (ha overload C++); poi hwcontext_d3d11va.h lo trova già incluso
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
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

// --- Scritte: stesso risultato di transformTextImage + blendTextOver (CPU) ---

namespace {

// Una scritta premoltiplicata di prova: un riquadro con colore e trasparenza variabili e un bordo pieno.
TextImage sampleText(int x, int y, int w, int h)
{
    TextImage text;
    text.x = x;
    text.y = y;
    text.width = w;
    text.height = h;
    text.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const bool border = i < 3 || j < 3 || i >= w - 3 || j >= h - 3;
            const int alpha = border ? 255 : 90 + (i * 120) / w;
            std::uint8_t* px = &text.rgba[(static_cast<std::size_t>(j) * w + i) * 4];
            px[0] = static_cast<std::uint8_t>(alpha * (border ? 255 : 200) / 255);
            px[1] = static_cast<std::uint8_t>(alpha * (border ? 255 : 60 + (j * 100) / h) / 255);
            px[2] = static_cast<std::uint8_t>(alpha * (border ? 0 : 30) / 255);
            px[3] = static_cast<std::uint8_t>(alpha);
        }
    return text;
}

struct TextCase
{
    const char* name;
    int x, y;
    Transform transform;
    double opacity;
};

class GpuTextCompare : public ::testing::TestWithParam<int>
{
};

}  // namespace

TEST_P(GpuTextCompare, MatchesTheCpuBlend)
{
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessun dispositivo D3D11 in questo ambiente";
    std::vector<TextCase> cases;
    cases.push_back({"com'e", 20, 10, Transform{}, 1.0});
    cases.push_back({"con dissolvenza", 20, 10, Transform{}, 0.6});
    cases.push_back({"in parte fuori dal fotogramma", -14, 50, Transform{}, 1.0});
    {
        Transform t;
        t.scaleX = t.scaleY = 1.5;
        t.rotation = 20;
        t.posX = 5;
        t.posY = -3;
        cases.push_back({"scala, rotazione e posizione", 30, 20, t, 0.8});
    }
    {
        Transform t;
        t.scaleX = 0.6;
        t.scaleY = 0.9;
        t.flipHorizontal = true;
        cases.push_back({"rimpicciolita e specchiata", 30, 20, t, 1.0});
    }
    const TextCase& c = cases[static_cast<std::size_t>(GetParam())];

    constexpr int w = 128;
    constexpr int h = 72;
    auto converter = GpuFrameConverter::create(*device, w, h);
    ASSERT_NE(converter, nullptr);
    // Il fotogramma di partenza: BGRA per la GPU, lo stesso in RGBA per la CPU.
    const std::vector<std::uint8_t> bgra = patternBgra(w, h, 1);
    std::vector<std::uint8_t> rgba = bgra;
    for (std::size_t i = 0; i < rgba.size(); i += 4)
        std::swap(rgba[i], rgba[i + 2]);
    const TextImage text = sampleText(c.x, c.y, 60, 24);
    const double pivotX = text.x + text.width / 2.0;
    const double pivotY = text.y + text.height / 2.0;

    ASSERT_TRUE(converter->setCanvasBgra({bgra.data(), w * 4, w, h}));
    const std::shared_ptr<GpuTextImage> gpuText = converter->createText(text);
    ASSERT_NE(gpuText, nullptr);
    ASSERT_TRUE(converter->drawText(*gpuText, c.transform, pivotX, pivotY, c.opacity));
    std::vector<std::uint16_t> half;
    ASSERT_TRUE(converter->readRgbaHalf(half));

    blendTextOver(rgba.data(), w * 4, w, h, transformTextImage(text, c.transform, pivotX, pivotY, w, h), c.opacity);

    double total = 0.0;
    int worst = 0;
    for (int i = 0; i < w * h; ++i)
        for (int ch = 0; ch < 3; ++ch) {
            const int gpu = static_cast<int>(std::lround(std::clamp(halfToFloat(half[static_cast<std::size_t>(i) * 4 + ch]), 0.0f, 1.0f) * 255.0f));
            const int diff = std::abs(gpu - rgba[static_cast<std::size_t>(i) * 4 + ch]);
            total += diff;
            worst = std::max(worst, diff);
        }
    const double mean = total / (static_cast<double>(w) * h * 3);
    std::cout << "[gpu] scritta «" << c.name << "»: differenza dalla CPU media " << mean << ", massima " << worst << std::endl;
    EXPECT_LT(mean, 1.0) << c.name;
}

INSTANTIATE_TEST_SUITE_P(Cases, GpuTextCompare, ::testing::Range(0, 5));

// --- Transizioni: stesso risultato di Transition::blend (CPU) ---

TEST(GpuTransition, EveryTransitionInTheRegistryHasAShader)
{
    // Aggiungere una transizione alla CPU senza il suo shader fa fallire questa prova (e l'esportazione ripiegherebbe sulla CPU).
    for (const Transition* transition : transitionRegistry())
        EXPECT_TRUE(GpuFrameConverter::supportsTransition(transition->id())) << transition->id();
    EXPECT_FALSE(GpuFrameConverter::supportsTransition("inesistente"));
}

TEST(GpuTransition, MatchesTheCpuTransitions)
{
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessun dispositivo D3D11 in questo ambiente";
    constexpr int w = 96;
    constexpr int h = 54;
    auto from = GpuFrameConverter::create(*device, w, h);
    auto to = GpuFrameConverter::create(*device, w, h);
    ASSERT_NE(from, nullptr);
    ASSERT_NE(to, nullptr);
    const std::vector<std::uint8_t> a = patternBgra(w, h, 0);
    const std::vector<std::uint8_t> b = patternBgra(w, h, 1);
    // La CPU lavora in RGBA: stessi byte con rosso e blu scambiati.
    std::vector<std::uint8_t> aRgba = a;
    std::vector<std::uint8_t> bRgba = b;
    for (std::size_t i = 0; i < aRgba.size(); i += 4) {
        std::swap(aRgba[i], aRgba[i + 2]);
        std::swap(bRgba[i], bRgba[i + 2]);
    }

    for (const Transition* transition : transitionRegistry()) {
        const std::vector<TransitionDirection> directions =
            transition->usesDirection()
                ? std::vector<TransitionDirection>{TransitionDirection::Left, TransitionDirection::Right, TransitionDirection::Up, TransitionDirection::Down}
                : std::vector<TransitionDirection>{TransitionDirection::Left};
        for (const TransitionDirection direction : directions)
            for (const double progress : {0.0, 0.2, 0.5, 0.73, 1.0}) {
                TransitionParams params;
                params.direction = direction;
                ASSERT_TRUE(from->setCanvasBgra({a.data(), w * 4, w, h}));
                ASSERT_TRUE(to->setCanvasBgra({b.data(), w * 4, w, h}));
                ASSERT_TRUE(from->blendTransition(*to, transition->id(), progress, params)) << transition->id();
                std::vector<std::uint16_t> half;
                ASSERT_TRUE(from->readRgbaHalf(half));

                std::vector<std::uint8_t> cpu(static_cast<std::size_t>(w) * h * 4);
                transition->blend({aRgba.data(), w, h, w * 4}, {bRgba.data(), w, h, w * 4}, progress, params, {cpu.data(), w * 4});
                double total = 0.0;
                for (int i = 0; i < w * h; ++i)
                    for (int ch = 0; ch < 3; ++ch) {
                        const int gpu = static_cast<int>(std::lround(std::clamp(halfToFloat(half[static_cast<std::size_t>(i) * 4 + ch]), 0.0f, 1.0f) * 255.0f));
                        total += std::abs(gpu - cpu[static_cast<std::size_t>(i) * 4 + ch]);
                    }
                const double mean = total / (static_cast<double>(w) * h * 3);
                EXPECT_LT(mean, 0.6) << transition->id() << " direzione " << static_cast<int>(direction) << " a " << progress;
            }
    }
}

// --- GpuCompositor: stessi fotogrammi di TextFrameBlender (CPU), con ripiego sulla CPU ---

namespace {

Frame smoothYuv420p(int w, int h)
{
    Frame frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = w;
    frame->height = h;
    frame->sample_aspect_ratio = {1, 1};
    frame->color_range = AVCOL_RANGE_MPEG;
    EXPECT_GE(av_frame_get_buffer(frame.get(), 32), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            frame->data[0][y * frame->linesize[0] + x] = static_cast<std::uint8_t>(60 + (x * 80) / w + (y * 50) / h + (((x / 16) + (y / 16)) % 2) * 20);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            frame->data[1][y * frame->linesize[1] + x] = static_cast<std::uint8_t>(108 + (x * 40) / (w / 2));
            frame->data[2][y * frame->linesize[2] + x] = static_cast<std::uint8_t>(148 - (y * 40) / (h / 2));
        }
    return frame;
}

// Differenza media fra due piani (8 bit).
double planeDiff(const AVFrame* a, const AVFrame* b, int plane, int w, int h)
{
    double total = 0.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            total += std::abs(a->data[plane][y * a->linesize[plane] + x] - b->data[plane][y * b->linesize[plane] + x]);
    return total / (static_cast<double>(w) * h);
}

void expectSameFrames(const AVFrame* gpu, const AVFrame* cpu, int w, int h, double maxY, double maxC, const char* what)
{
    ASSERT_NE(gpu, nullptr);
    ASSERT_NE(cpu, nullptr);
    const double y = planeDiff(gpu, cpu, 0, w, h);
    const double u = planeDiff(gpu, cpu, 1, w / 2, h / 2);
    const double v = planeDiff(gpu, cpu, 2, w / 2, h / 2);
    std::cout << "[gpu] compositore «" << what << "»: differenza dalla CPU Y " << y << ", U " << u << ", V " << v << std::endl;
    EXPECT_LT(y, maxY) << what;
    EXPECT_LT(u, maxC) << what;
    EXPECT_LT(v, maxC) << what;
}

}  // namespace

TEST(GpuCompositor, BlendWithTextMatchesTheCpuBlender)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string why;
    const auto gpu = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why);
    if (!gpu)
        GTEST_SKIP() << "nessuna GPU: " << why;
    EXPECT_TRUE(gpu->isHardware());
    TextFrameBlender cpu(w, h, AV_PIX_FMT_YUV420P);
    const Frame source = smoothYuv420p(w, h);
    const TextImage text = sampleText(20, 10, 60, 24);
    BlendedText blended;
    blended.image = &text;
    blended.opacity = 0.7;
    expectSameFrames(gpu->blend(source.get(), {blended}), cpu.blend(source.get(), {blended}), w, h, 2.0, 2.5, "blend con scritta");
}

TEST(GpuCompositor, TransformedWithAnimatedTextMatchesTheCpuBlender)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string why;
    const auto gpu = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why);
    if (!gpu)
        GTEST_SKIP() << "nessuna GPU: " << why;
    TextFrameBlender cpu(w, h, AV_PIX_FMT_YUV420P);
    const Frame source = smoothYuv420p(w, h);
    Transform t;
    t.scaleX = t.scaleY = 0.7;
    t.rotation = 15;
    t.posX = 6;
    const TextImage text = sampleText(30, 20, 50, 20);
    BlendedText blended;  // la scritta si trasforma dove la si compone: la GPU a ogni fotogramma, la CPU con transformTextImage
    blended.base = &text;
    blended.transform.scaleX = blended.transform.scaleY = 1.3;
    blended.transform.rotation = 25;
    blended.pivotX = 55;
    blended.pivotY = 30;
    blended.opacity = 0.9;
    blended.cacheId = 7;
    expectSameFrames(gpu->blendTransformed(source.get(), t, {blended}), cpu.blendTransformed(source.get(), t, {blended}), w, h, 3.0, 3.5,
                     "trasformato con scritta animata");
    // Lo stesso con la scritta già nella scheda video (cacheId): stesso risultato.
    expectSameFrames(gpu->blendTransformed(source.get(), t, {blended}), cpu.blendTransformed(source.get(), t, {blended}), w, h, 3.0, 3.5,
                     "trasformato, scritta dalla cache");
}

TEST(GpuCompositor, RasterAndLayersMatchTheCpuBlender)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string why;
    const auto gpu = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why);
    if (!gpu)
        GTEST_SKIP() << "nessuna GPU: " << why;
    TextFrameBlender cpu(w, h, AV_PIX_FMT_YUV420P);
    const std::vector<std::uint8_t> raster = patternBgra(w, h, 1);
    VideoFrame layerFrame;
    layerFrame.width = 64;
    layerFrame.height = 36;
    layerFrame.stride = 64 * 4;
    layerFrame.pixels = patternBgra(64, 36, 0);
    Transform layerTransform;
    layerTransform.scaleX = layerTransform.scaleY = 0.8;
    layerTransform.posX = 20;
    layerTransform.opacity = 0.8;
    const TextImage under = sampleText(5, 5, 40, 16);
    const TextImage over = sampleText(60, 40, 40, 16);
    BlendedText belowLayer;
    belowLayer.image = &under;
    belowLayer.level = 0;
    BlendedText aboveLayer;
    aboveLayer.image = &over;
    aboveLayer.level = 1;
    AVFrame* props = av_frame_alloc();
    props->pts = 42;
    gpu->setLayers({{&layerFrame, layerTransform, 1}});
    cpu.setLayers({{&layerFrame, layerTransform, 1}});
    const AVFrame* a = gpu->blendRaster(raster.data(), w * 4, w, h, {belowLayer, aboveLayer}, props);
    const AVFrame* b = cpu.blendRaster(raster.data(), w * 4, w, h, {belowLayer, aboveLayer}, props);
    expectSameFrames(a, b, w, h, 2.5, 3.0, "transizione, livello e scritte");
    EXPECT_EQ(a->pts, 42);
    av_frame_free(&props);
}

TEST(GpuCompositor, TransitionOfTwoTransformedSidesMatchesTheCpuFusion)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string why;
    const auto gpu = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why);
    if (!gpu)
        GTEST_SKIP() << "nessuna GPU: " << why;
    TextFrameBlender cpu(w, h, AV_PIX_FMT_YUV420P);
    TransitionRenderer renderer;
    renderer.setBox(w, h);
    auto makeSide = [&](int seed) {
        auto frame = std::make_shared<VideoFrame>();
        frame->width = w;
        frame->height = h;
        frame->stride = w * 4;
        frame->pixels = patternBgra(w, h, seed);
        return frame;
    };
    TransitionSides sides;
    sides.from = makeSide(0);
    sides.to = makeSide(1);
    sides.toTransform.scaleX = sides.toTransform.scaleY = 0.75;
    sides.toTransform.posX = 10;
    AVFrame* props = av_frame_alloc();
    props->pts = 7;
    const TextImage text = sampleText(10, 8, 40, 16);
    BlendedText blended;
    blended.image = &text;
    for (const char* id : {"dissolvenza", "dissolvenza-nero", "dissolvenza-bianco", "scorrimento", "tendina", "copertura", "scopertura"}) {
        TransitionJob job;
        job.typeId = id;
        job.params.direction = TransitionDirection::Left;
        sides.progress = 0.4;
        TransitionBlend request;
        request.typeId = job.typeId;
        request.params = job.params;
        request.progress = sides.progress;
        request.from = sides.from.get();
        request.to = sides.to.get();
        request.fromTransform = sides.fromTransform;
        request.toTransform = sides.toTransform;
        const AVFrame* a = gpu->blendTransition(request, {blended}, props);
        const std::shared_ptr<VideoFrame> fused = renderer.fuse(job, sides);
        ASSERT_NE(fused, nullptr);
        const AVFrame* b = cpu.blendRaster(fused->pixels.data(), fused->stride, w, h, {blended}, props);
        expectSameFrames(a, b, w, h, 3.0, 3.5, id);
        EXPECT_EQ(a->pts, 7);
    }
    // Un effetto senza shader: la GPU rinuncia (si fa con la CPU) e la CPU non offre la fusione.
    TransitionBlend unknown;
    unknown.typeId = "inesistente";
    unknown.from = sides.from.get();
    unknown.to = sides.to.get();
    EXPECT_EQ(gpu->blendTransition(unknown, {}, props), nullptr);
    av_frame_free(&props);
}

TEST(GpuCompositor, UnsupportedSourceFormatFallsBackToTheCpuForThatFrameOnly)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string why;
    const auto gpu = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why);
    if (!gpu)
        GTEST_SKIP() << "nessuna GPU: " << why;
    TextFrameBlender cpu(w, h, AV_PIX_FMT_YUV420P);
    Frame tenBit(av_frame_alloc());
    tenBit->format = AV_PIX_FMT_YUV420P10LE;
    tenBit->width = w;
    tenBit->height = h;
    tenBit->sample_aspect_ratio = {1, 1};
    ASSERT_GE(av_frame_get_buffer(tenBit.get(), 32), 0);
    for (int p = 0; p < 3; ++p) {
        const int pw = p == 0 ? w : w / 2;
        const int ph = p == 0 ? h : h / 2;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
                reinterpret_cast<std::uint16_t*>(tenBit->data[p] + y * tenBit->linesize[p])[x] = static_cast<std::uint16_t>(p == 0 ? 300 + x * 3 : 512);
    }
    const AVFrame* a = gpu->blend(tenBit.get(), {});
    const AVFrame* b = cpu.blend(tenBit.get(), {});
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(planeDiff(a, b, 0, w, h), 0.0);  // è il codice della CPU
    EXPECT_TRUE(gpu->isHardware());  // la GPU resta in uso per gli altri fotogrammi
    EXPECT_TRUE(gpu->fallbackReason().empty());
    // Un fotogramma yuv420p subito dopo torna sulla GPU.
    const Frame source = smoothYuv420p(w, h);
    expectSameFrames(gpu->blend(source.get(), {}), cpu.blend(source.get(), {}), w, h, 2.0, 2.5, "dopo il ripiego di un fotogramma");
}

TEST(GpuCompositor, Nv12OutputWorksToo)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string why;
    const auto gpu = GpuCompositor::create(w, h, AV_PIX_FMT_NV12, true, &why);
    if (!gpu)
        GTEST_SKIP() << "nessuna GPU: " << why;
    const Frame source = smoothYuv420p(w, h);
    const AVFrame* out = gpu->blend(source.get(), {});
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->format, AV_PIX_FMT_NV12);
    // Senza scritte né trasformazioni il fotogramma torna com'era (a meno degli arrotondamenti).
    double total = 0.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            total += std::abs(out->data[0][y * out->linesize[0] + x] - source->data[0][y * source->linesize[0] + x]);
    EXPECT_LT(total / (static_cast<double>(w) * h), 2.0);
}

TEST(GpuCompositor, FactoryNeverGivesTheSoftwareRasterizerAndAcceptsOnlyEightBit420)
{
    // Senza una scheda vera (come in CI) la fabbrica dà la CPU; con 10 bit sempre la CPU.
    const auto eight = makeFrameCompositor(128, 72, AV_PIX_FMT_YUV420P);
    ASSERT_NE(eight, nullptr);
    if (!eight->isHardware())
        EXPECT_STREQ(eight->name(), "CPU");
    const auto ten = makeFrameCompositor(128, 72, AV_PIX_FMT_YUV420P10LE);
    ASSERT_NE(ten, nullptr);
    EXPECT_FALSE(ten->isHardware());
    const auto forced = makeFrameCompositor(128, 72, AV_PIX_FMT_YUV420P, false);
    ASSERT_NE(forced, nullptr);
    EXPECT_FALSE(forced->isHardware());
}

// --- Fotogrammi della scheda video (D3D11) letti direttamente dal compositore ---

namespace {

struct BufferRefDeleter
{
    void operator()(AVBufferRef* ref) const { av_buffer_unref(&ref); }
};
using BufferRef = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

// Un fotogramma D3D11 (NV12) del dispositivo dato, con dentro `source` (yuv420p, riordinato in NV12 da swscale e caricato con FFmpeg).
Frame uploadToD3d11(AVBufferRef* device, const AVFrame* source, BufferRef& framesOut)
{
    BufferRef frames(av_hwframe_ctx_alloc(device));
    if (!frames)
        return nullptr;
    auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
    context->format = AV_PIX_FMT_D3D11;
    context->sw_format = AV_PIX_FMT_NV12;
    context->width = source->width;
    context->height = source->height;
    context->initial_pool_size = 2;
    auto* hardware = static_cast<AVD3D11VAFramesContext*>(context->hwctx);
    hardware->BindFlags = D3D11_BIND_SHADER_RESOURCE;  // fuori dal decoder, per le prove
    if (av_hwframe_ctx_init(frames.get()) < 0)
        return nullptr;
    Frame nv12(av_frame_alloc());
    nv12->format = AV_PIX_FMT_NV12;
    nv12->width = source->width;
    nv12->height = source->height;
    if (av_frame_get_buffer(nv12.get(), 32) < 0)
        return nullptr;
    SwsContext* sws = sws_getContext(source->width, source->height, AV_PIX_FMT_YUV420P, source->width, source->height, AV_PIX_FMT_NV12,
                                     SWS_POINT, nullptr, nullptr, nullptr);
    if (sws == nullptr)
        return nullptr;
    sws_scale(sws, source->data, source->linesize, 0, source->height, nv12->data, nv12->linesize);
    sws_freeContext(sws);
    Frame hw(av_frame_alloc());
    if (av_hwframe_get_buffer(frames.get(), hw.get(), 0) < 0)
        return nullptr;
    if (av_hwframe_transfer_data(hw.get(), nv12.get(), 0) < 0)
        return nullptr;
    hw->sample_aspect_ratio = {1, 1};
    hw->color_range = AVCOL_RANGE_MPEG;
    framesOut = std::move(frames);
    return hw;
}

}  // namespace

#ifdef _WIN32
TEST(GpuCompositor, DecodedD3d11FrameIsReadDirectlyAndMatchesTheMemoryPath)
{
    constexpr int w = 128;
    constexpr int h = 72;
    std::string error;
    std::shared_ptr<GpuDevice> device(GpuDevice::create(true, &error));
    if (!device)
        GTEST_SKIP() << "nessuna GPU: " << error;
    BufferRef ffmpegDevice(device->createFfmpegDevice());
    if (!ffmpegDevice)
        GTEST_SKIP() << "il dispositivo non si dà a FFmpeg";
    const Frame source = smoothYuv420p(w, h);
    BufferRef frames;
    const Frame hw = uploadToD3d11(ffmpegDevice.get(), source.get(), frames);
    if (!hw)
        GTEST_SKIP() << "fotogrammi D3D11 non creabili su questo dispositivo";
    std::string why;
    const auto direct = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why, device);
    ASSERT_NE(direct, nullptr) << why;
    const auto viaMemory = GpuCompositor::create(w, h, AV_PIX_FMT_YUV420P, true, &why);
    ASSERT_NE(viaMemory, nullptr) << why;
    Transform transform;
    transform.scaleX = 0.6;
    transform.scaleY = 0.6;
    transform.rotation = 12.0;
    const AVFrame* a = direct->blendTransformed(hw.get(), transform, {});
    const AVFrame* b = viaMemory->blendTransformed(hw.get(), transform, {});  // l'altro compositore ha un altro dispositivo: passa dalla memoria
    expectSameFrames(a, b, w, h, 1.0, 1.0, "fotogramma D3D11 diretto");
    EXPECT_EQ(direct->timing().directFrames, 1);
    EXPECT_EQ(viaMemory->timing().directFrames, 0);
    EXPECT_TRUE(direct->isHardware());
}
#endif

#ifdef _WIN32
// Il compositore scrive il fotogramma finito direttamente in una texture NV12 (quella dell'encoder): stesso risultato di downloadNv12.
TEST(GpuColor, RendersNv12DirectlyIntoATextureLikeTheDownload)
{
    constexpr int w = 64;
    constexpr int h = 48;
    const auto device = makeDevice();
    if (!device)
        GTEST_SKIP() << "nessuna GPU";
    const auto converter = GpuFrameConverter::create(*device, w, h);
    ASSERT_NE(converter, nullptr);
    const Frame yuv = smoothYuv420p(w, h);
    Frame nv12(av_frame_alloc());
    nv12->format = AV_PIX_FMT_NV12;
    nv12->width = w;
    nv12->height = h;
    ASSERT_GE(av_frame_get_buffer(nv12.get(), 32), 0);
    for (int y = 0; y < h; ++y)
        std::memcpy(nv12->data[0] + y * nv12->linesize[0], yuv->data[0] + y * yuv->linesize[0], w);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            nv12->data[1][y * nv12->linesize[1] + 2 * x] = yuv->data[1][y * yuv->linesize[1] + x];
            nv12->data[1][y * nv12->linesize[1] + 2 * x + 1] = yuv->data[2][y * yuv->linesize[2] + x];
        }
    const GpuColorSpace colors = GpuColorSpace::bt601(false);
    ASSERT_TRUE(converter->uploadNv12(nv12.get(), colors));
    Frame reference(av_frame_alloc());
    reference->format = AV_PIX_FMT_NV12;
    reference->width = w;
    reference->height = h;
    ASSERT_GE(av_frame_get_buffer(reference.get(), 32), 0);
    ASSERT_TRUE(converter->downloadNv12(reference.get(), colors));

    ID3D11Device* d3d = device->impl().device.Get();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
    if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &target)))
        GTEST_SKIP() << "texture NV12 con scrittura non creabile su questo dispositivo";
    std::string why;
    if (!converter->renderNv12Texture(target.Get(), 0, colors, &why)) {
        if (why.find("non creabile") != std::string::npos)
            GTEST_SKIP() << why;
        FAIL() << why;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
    if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &stage)))
        GTEST_SKIP() << "texture NV12 di lettura non creabile su questo dispositivo";
    device->impl().context->CopyResource(stage.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(device->impl().context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto* data = static_cast<const std::uint8_t*>(mapped.pData);
    double diffY = 0.0;
    double diffUV = 0.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            diffY += std::abs(data[y * mapped.RowPitch + x] - reference->data[0][y * reference->linesize[0] + x]);
    const std::uint8_t* uv = data + static_cast<std::size_t>(mapped.RowPitch) * h;
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x)
            diffUV += std::abs(uv[y * mapped.RowPitch + x] - reference->data[1][y * reference->linesize[1] + x]);
    device->impl().context->Unmap(stage.Get(), 0);
    std::cout << "[gpu] scrittura diretta in NV12: differenza dal download Y " << diffY / (w * h) << ", UV " << diffUV / (w * h / 2) << std::endl;
    EXPECT_LT(diffY / (w * h), 0.5);
    EXPECT_LT(diffUV / (w * h / 2), 0.5);
}
#endif
