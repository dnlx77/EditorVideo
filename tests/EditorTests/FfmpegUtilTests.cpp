#include "EditorCore/FfmpegUtil.h"

#include <gtest/gtest.h>

using namespace editor::core::detail;

namespace {

AVFrame frameOf(int width, int height, AVRational sar = {1, 1})
{
    AVFrame frame = {};
    frame.width = width;
    frame.height = height;
    frame.sample_aspect_ratio = sar;
    return frame;
}

}  // namespace

TEST(FfmpegUtil, EvenSizeIsEvenAndAtLeastTwo)
{
    EXPECT_EQ(evenSize(100.0), 100);
    EXPECT_EQ(evenSize(101.0), 102);  // arrotonda al pari più vicino
    EXPECT_EQ(evenSize(99.0), 100);
    EXPECT_EQ(evenSize(0.2), 2);
    EXPECT_EQ(evenSize(-5.0), 2);
}

TEST(FfmpegUtil, FitIntoSameAspectFillsTheBox)
{
    const AVFrame frame = frameOf(1280, 720);
    const FittedSize fit = fitInto(&frame, 1920, 1080);
    EXPECT_EQ(fit.width, 1920);
    EXPECT_EQ(fit.height, 1080);
    EXPECT_EQ(fit.x, 0);
    EXPECT_EQ(fit.y, 0);
}

TEST(FfmpegUtil, FitIntoNarrowerImageGetsBarsOnTheSides)
{
    const AVFrame frame = frameOf(640, 480);  // 4:3 dentro un riquadro 16:9
    const FittedSize fit = fitInto(&frame, 1920, 1080);
    EXPECT_EQ(fit.height, 1080);
    EXPECT_EQ(fit.width, 1440);
    EXPECT_EQ(fit.x, 240);
    EXPECT_EQ(fit.y, 0);
}

TEST(FfmpegUtil, FitIntoWiderImageGetsBarsAboveAndBelow)
{
    const AVFrame frame = frameOf(1920, 800);  // 2.4:1 dentro un riquadro 16:9
    const FittedSize fit = fitInto(&frame, 1920, 1080);
    EXPECT_EQ(fit.width, 1920);
    EXPECT_EQ(fit.height, 800);
    EXPECT_EQ(fit.x, 0);
    EXPECT_EQ(fit.y, 140);
}

TEST(FfmpegUtil, FitIntoUsesPixelAspectRatioAndEvenPositions)
{
    // 960x1080 con pixel larghi il doppio = 16:9 di visualizzazione: riempie il riquadro.
    const AVFrame anamorphic = frameOf(960, 1080, {2, 1});
    const FittedSize full = fitInto(&anamorphic, 1920, 1080);
    EXPECT_EQ(full.width, 1920);
    EXPECT_EQ(full.height, 1080);
    // Posizioni sempre pari (il colore è sottocampionato).
    const AVFrame odd = frameOf(500, 300);
    const FittedSize fit = fitInto(&odd, 1000, 700);
    EXPECT_EQ(fit.x % 2, 0);
    EXPECT_EQ(fit.y % 2, 0);
    EXPECT_EQ(fit.width % 2, 0);
    EXPECT_EQ(fit.height % 2, 0);
}

TEST(FfmpegUtil, NormalizeJpegRangeMapsJFormatsToFullRange)
{
    AVPixelFormat format = AV_PIX_FMT_YUVJ420P;
    bool fullRange = false;
    normalizeJpegRange(format, fullRange);
    EXPECT_EQ(format, AV_PIX_FMT_YUV420P);
    EXPECT_TRUE(fullRange);

    format = AV_PIX_FMT_YUVJ444P;
    fullRange = false;
    normalizeJpegRange(format, fullRange);
    EXPECT_EQ(format, AV_PIX_FMT_YUV444P);
    EXPECT_TRUE(fullRange);

    format = AV_PIX_FMT_NV12;
    fullRange = false;
    normalizeJpegRange(format, fullRange);
    EXPECT_EQ(format, AV_PIX_FMT_NV12);  // gli altri non cambiano
    EXPECT_FALSE(fullRange);
}

TEST(FfmpegUtil, ChooseDecoderFormatPrefersHardwareThenFirstSoftware)
{
    const AVPixelFormat offered[] = {AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    bool hardware = false;
    EXPECT_EQ(chooseDecoderFormat(offered, AV_PIX_FMT_D3D11, &hardware), AV_PIX_FMT_D3D11);
    EXPECT_TRUE(hardware);

    // La GPU non gestisce il flusso: il formato D3D11 non è fra quelli offerti, si prende il primo software.
    const AVPixelFormat noGpu[] = {AV_PIX_FMT_DXVA2_VLD, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    EXPECT_EQ(chooseDecoderFormat(noGpu, AV_PIX_FMT_D3D11, &hardware), AV_PIX_FMT_YUV420P);
    EXPECT_FALSE(hardware);

    // Senza formato voluto (come il decoder dei proxy) si prende sempre il primo software.
    EXPECT_EQ(chooseDecoderFormat(offered, AV_PIX_FMT_NONE), AV_PIX_FMT_NV12);

    const AVPixelFormat onlyHardware[] = {AV_PIX_FMT_D3D11, AV_PIX_FMT_NONE};
    EXPECT_EQ(chooseDecoderFormat(onlyHardware, AV_PIX_FMT_NONE), AV_PIX_FMT_NONE);
}
