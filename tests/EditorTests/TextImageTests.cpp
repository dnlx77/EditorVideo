#include "EditorCore/TextImage.h"

#include <gtest/gtest.h>

#include <cstdint>

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

namespace {

int alphaAt(const TextImage& image, int x, int y)
{
    if (x < image.x || y < image.y || x >= image.x + image.width || y >= image.y + image.height)
        return 0;
    return image.rgba[(static_cast<std::size_t>(y - image.y) * image.width + (x - image.x)) * 4 + 3];
}

}  // namespace

TEST(TextImageTransform, NoMovementGivesTheSameImage)
{
    const TextImage text = solid(10, 10, 20, 10, 255, 255, 255, 255);
    Transform t;
    t.opacity = 0.5;  // l'opacità non è affare di questa funzione
    const TextImage same = transformTextImage(text, t, 20, 15, 100, 50);
    EXPECT_EQ(same.x, 10);
    EXPECT_EQ(same.width, 20);
    EXPECT_EQ(same.rgba, text.rgba);
}

TEST(TextImageTransform, ScaleGrowsAroundThePivotAndShiftMoves)
{
    const TextImage text = solid(40, 20, 20, 10, 255, 255, 255, 255);  // centro (50, 25)
    Transform t;
    t.scaleX = t.scaleY = 2.0;
    const TextImage big = transformTextImage(text, t, 50, 25, 100, 50);
    EXPECT_EQ(alphaAt(big, 50, 25), 255);
    EXPECT_EQ(alphaAt(big, 33, 19), 255);   // 10 px di raggio -> 20 px: arriva fino a (30, 15) (i bordi sono morbidi: si guarda dentro)
    EXPECT_EQ(alphaAt(big, 67, 31), 255);
    EXPECT_EQ(alphaAt(big, 25, 25), 0);
    EXPECT_EQ(alphaAt(big, 75, 25), 0);
    // Spostamento: tutto 30 px a destra.
    Transform shift;
    shift.posX = 30.0;
    const TextImage moved = transformTextImage(text, shift, 50, 25, 100, 50);
    EXPECT_EQ(alphaAt(moved, 45, 25), 0);
    EXPECT_EQ(alphaAt(moved, 75, 25), 255);
    EXPECT_EQ(alphaAt(moved, 87, 27), 255);
}

TEST(TextImageTransform, RotationTurnsAndFlipMirrorsAroundThePivot)
{
    // Un riquadro largo 40, alto 10, con il lato destro pieno e il sinistro vuoto non si vede da una sola alfa uniforme: si usa
    // un riquadro con una "bandierina" a destra (alfa solo nella metà destra).
    TextImage text = solid(30, 20, 40, 10, 0, 0, 0, 0);
    for (int y = 0; y < 10; ++y) {
        for (int x = 20; x < 40; ++x)
            text.rgba[(static_cast<std::size_t>(y) * 40 + x) * 4 + 3] = 255;
    }
    // centro (50, 25); la metà piena sta a destra del centro (x 50..70)
    Transform turn;
    turn.rotation = 90.0;  // in senso orario: la destra va in basso
    const TextImage rotated = transformTextImage(text, turn, 50, 25, 100, 60);
    EXPECT_EQ(alphaAt(rotated, 50, 40), 255);  // sotto il centro
    EXPECT_EQ(alphaAt(rotated, 50, 10), 0);    // sopra: vuoto
    EXPECT_EQ(alphaAt(rotated, 70, 25), 0);
    Transform flip;
    flip.flipHorizontal = true;
    const TextImage mirrored = transformTextImage(text, flip, 50, 25, 100, 60);
    EXPECT_EQ(alphaAt(mirrored, 35, 25), 255);  // la metà piena ora è a sinistra
    EXPECT_EQ(alphaAt(mirrored, 65, 25), 0);
}

TEST(TextImageTransform, PartOutsideTheFrameIsCutAndAFullyOutsideTextIsEmpty)
{
    const TextImage text = solid(40, 20, 20, 10, 255, 255, 255, 255);
    Transform t;
    t.posX = 45.0;  // metà fuori dal fotogramma largo 100
    const TextImage cut = transformTextImage(text, t, 50, 25, 100, 50);
    EXPECT_FALSE(cut.empty());
    EXPECT_LE(cut.x + cut.width, 100);
    Transform away;
    away.posX = 500.0;
    EXPECT_TRUE(transformTextImage(text, away, 50, 25, 100, 50).empty());
}
