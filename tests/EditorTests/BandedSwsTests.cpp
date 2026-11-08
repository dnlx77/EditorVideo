#include "EditorCore/BandedSws.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace editor::core;

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

// Immagine RGBA con sfumature e un rettangolo a bordi netti (i bordi sono il caso difficile per il colore).
std::vector<std::uint8_t> makeRgba()
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4];
            const bool box = x > 60 && x < 250 && y > 30 && y < 200;
            p[0] = box ? 230 : static_cast<std::uint8_t>(x * 255 / kWidth);
            p[1] = box ? 40 : static_cast<std::uint8_t>(y * 255 / kHeight);
            p[2] = static_cast<std::uint8_t>((x + y) & 255);
            p[3] = 255;
        }
    }
    return rgba;
}

}  // namespace

TEST(BandedSws, BandsCoverTheWholeImageWithEvenEdges)
{
    for (int height : {100, 240, 1080, 2160, 1081}) {
        const int count = bandCount(height);
        const std::vector<int> edges = bandEdges(height, count);
        ASSERT_EQ(static_cast<int>(edges.size()), count + 1);
        EXPECT_EQ(edges.front(), 0);
        EXPECT_EQ(edges.back(), height);
        for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
            EXPECT_LT(edges[i], edges[i + 1]);
            EXPECT_EQ(edges[i] % 2, 0);
        }
    }
}

TEST(BandedSws, RgbaToYuvMatchesWholeImageConversionExceptNearBandEdges)
{
    const std::vector<std::uint8_t> rgba = makeRgba();
    const std::uint8_t* src[4] = {rgba.data(), nullptr, nullptr, nullptr};
    const int srcStride[4] = {kWidth * 4, 0, 0, 0};

    const std::size_t lumaSize = static_cast<std::size_t>(kWidth) * kHeight;
    std::vector<std::uint8_t> whole(lumaSize * 3 / 2);
    std::vector<std::uint8_t> banded(lumaSize * 3 / 2);
    const auto planes = [&](std::vector<std::uint8_t>& v, std::uint8_t* out[4]) {
        out[0] = v.data();
        out[1] = v.data() + lumaSize;
        out[2] = v.data() + lumaSize * 5 / 4;
        out[3] = nullptr;
    };
    const int dstStride[4] = {kWidth, kWidth / 2, kWidth / 2, 0};

    std::unique_ptr<SwsContext, detail::SwsDeleter> plain(
        sws_getContext(kWidth, kHeight, AV_PIX_FMT_RGBA, kWidth, kHeight, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr));
    ASSERT_TRUE(plain);
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(plain.get(), coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
    std::uint8_t* wholePlanes[4];
    planes(whole, wholePlanes);
    sws_scale(plain.get(), src, srcStride, 0, kHeight, wholePlanes, dstStride);

    BandedSws converter;
    ASSERT_TRUE(converter.prepare(kWidth, kHeight, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P, true, false));
    std::uint8_t* bandedPlanes[4];
    planes(banded, bandedPlanes);
    converter.convert(src, srcStride, bandedPlanes, dstStride);

    // Il bianco e nero è identico; il colore può cambiare di poco solo sulle righe a cavallo di due bande.
    int maxDifference = 0;
    for (std::size_t i = 0; i < whole.size(); ++i) {
        const int difference = std::abs(static_cast<int>(whole[i]) - static_cast<int>(banded[i]));
        if (i < lumaSize)
            EXPECT_EQ(difference, 0) << "luma " << i;
        maxDifference = std::max(maxDifference, difference);
    }
    EXPECT_LE(maxDifference, 12);
}

TEST(BandedSws, YuvToRgbaIsIdenticalToWholeImageConversion)
{
    const std::size_t lumaSize = static_cast<std::size_t>(kWidth) * kHeight;
    std::vector<std::uint8_t> yuv(lumaSize * 3 / 2);
    for (std::size_t i = 0; i < yuv.size(); ++i)
        yuv[i] = static_cast<std::uint8_t>((i * 7 + i / kWidth) & 255);
    const std::uint8_t* src[4] = {yuv.data(), yuv.data() + lumaSize, yuv.data() + lumaSize * 5 / 4, nullptr};
    const int srcStride[4] = {kWidth, kWidth / 2, kWidth / 2, 0};

    std::vector<std::uint8_t> whole(lumaSize * 4);
    std::vector<std::uint8_t> banded(lumaSize * 4);
    std::uint8_t* wholePlanes[4] = {whole.data(), nullptr, nullptr, nullptr};
    std::uint8_t* bandedPlanes[4] = {banded.data(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {kWidth * 4, 0, 0, 0};

    std::unique_ptr<SwsContext, detail::SwsDeleter> plain(
        sws_getContext(kWidth, kHeight, AV_PIX_FMT_YUV420P, kWidth, kHeight, AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
    ASSERT_TRUE(plain);
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(plain.get(), coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    sws_scale(plain.get(), src, srcStride, 0, kHeight, wholePlanes, dstStride);

    BandedSws converter;
    ASSERT_TRUE(converter.prepare(kWidth, kHeight, AV_PIX_FMT_YUV420P, AV_PIX_FMT_RGBA, false, true));
    converter.convert(src, srcStride, bandedPlanes, dstStride);
    EXPECT_EQ(whole, banded);
}

TEST(BandedSws, PrepareAgainWithOtherSizeWorks)
{
    BandedSws converter;
    ASSERT_TRUE(converter.prepare(64, 64, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P, true, false));
    ASSERT_TRUE(converter.prepare(kWidth, kHeight, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P, true, false));
    EXPECT_FALSE(converter.prepare(0, 10, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P, true, false));
}

TEST(ParallelFor, RunsEveryTaskOnceAlsoFromTwoThreadsAtTheSameTime)
{
    std::atomic<long> sum{0};
    const auto work = [&](int tasks, int rounds) {
        for (int r = 0; r < rounds; ++r)
            parallelFor(tasks, [&](int i) { sum += i + 1; });
    };
    std::thread other([&] { work(13, 500); });
    work(37, 500);
    other.join();
    EXPECT_EQ(sum.load(), 500L * (37 * 38 / 2) + 500L * (13 * 14 / 2));
    parallelFor(0, [&](int) { FAIL(); });
}
