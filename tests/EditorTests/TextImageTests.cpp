#include "EditorCore/TextImage.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

TextImage solid(int x, int y, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    TextImage image;
    image.x = x;
    image.y = y;
    image.width = w;
    image.height = h;
    for (int i = 0; i < w * h; ++i) {
        image.rgba.push_back(r);
        image.rgba.push_back(g);
        image.rgba.push_back(b);
        image.rgba.push_back(a);
    }
    return image;
}

std::vector<std::uint8_t> gray(int w, int h, std::uint8_t value)
{
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(w) * h * 4, value);
    for (std::size_t i = 3; i < frame.size(); i += 4)
        frame[i] = 255;
    return frame;
}

}  // namespace

TEST(TextImage, OpaquePixelsReplaceTheFrame)
{
    auto frame = gray(4, 4, 100);
    blendTextOver(frame.data(), 16, 4, 4, solid(1, 1, 2, 2, 255, 0, 0, 255));
    const auto at = [&](int x, int y) { return &frame[(static_cast<std::size_t>(y) * 4 + x) * 4]; };
    EXPECT_EQ(at(1, 1)[0], 255);
    EXPECT_EQ(at(1, 1)[1], 0);
    EXPECT_EQ(at(2, 2)[0], 255);
    EXPECT_EQ(at(0, 0)[0], 100);
    EXPECT_EQ(at(3, 3)[0], 100);
    EXPECT_EQ(at(1, 1)[3], 255);  // l'alfa del fotogramma non cambia
}

TEST(TextImage, HalfTransparentBlendsPremultiplied)
{
    auto frame = gray(1, 1, 100);
    blendTextOver(frame.data(), 4, 1, 1, solid(0, 0, 1, 1, 0, 0, 0, 128));  // nero a metà
    EXPECT_NEAR(frame[0], 50, 1);
    auto white = gray(1, 1, 100);
    blendTextOver(white.data(), 4, 1, 1, solid(0, 0, 1, 1, 128, 128, 128, 128));  // bianco a metà (premoltiplicato)
    EXPECT_NEAR(white[0], 178, 1);
}

TEST(TextImage, ClipsToTheFrameAndRespectsStride)
{
    auto frame = gray(2, 2, 0);
    blendTextOver(frame.data(), 8, 2, 2, solid(-1, -1, 3, 3, 255, 255, 255, 255));
    for (std::size_t i = 0; i < frame.size(); i += 4)
        EXPECT_EQ(frame[i], 255);
    // Immagine tutta fuori, vuota o frame nullo: niente da fare, niente errori.
    auto other = gray(2, 2, 7);
    blendTextOver(other.data(), 8, 2, 2, solid(5, 5, 2, 2, 255, 255, 255, 255));
    blendTextOver(other.data(), 8, 2, 2, TextImage{});
    blendTextOver(nullptr, 8, 2, 2, solid(0, 0, 1, 1, 1, 1, 1, 255));
    EXPECT_EQ(other[0], 7);
    // Riga più larga del fotogramma (stride): le righe restano allineate.
    std::vector<std::uint8_t> padded(16 * 2, 0);
    blendTextOver(padded.data(), 16, 2, 2, solid(0, 1, 1, 1, 9, 9, 9, 255));
    EXPECT_EQ(padded[16], 9);
    EXPECT_EQ(padded[0], 0);
}
