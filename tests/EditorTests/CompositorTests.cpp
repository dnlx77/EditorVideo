#include "EditorCore/Compositor.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

using namespace editor::core;

namespace {

using Color = std::array<std::uint8_t, 3>;  // B, G, R
constexpr Color kBlack{0, 0, 0};
constexpr Color kRed{0, 0, 255};
constexpr Color kGreen{0, 255, 0};
constexpr Color kBlue{255, 0, 0};
constexpr Color kWhite{255, 255, 255};

struct Image
{
    int width;
    int height;
    std::vector<std::uint8_t> data;

    Image(int w, int h, Color fill = kBlack) : width(w), height(h), data(static_cast<std::size_t>(w) * h * 4)
    {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                set(x, y, fill);
    }
    void set(int x, int y, Color c)
    {
        std::uint8_t* p = &data[(static_cast<std::size_t>(y) * width + x) * 4];
        p[0] = c[0];
        p[1] = c[1];
        p[2] = c[2];
        p[3] = 255;
    }
    Color at(int x, int y) const
    {
        const std::uint8_t* p = &data[(static_cast<std::size_t>(y) * width + x) * 4];
        return {p[0], p[1], p[2]};
    }
    BgraImage view() const { return {data.data(), width * 4, width, height}; }
};

// Metà sinistra rossa, metà destra verde.
Image halves(int w, int h)
{
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.set(x, y, x < w / 2 ? kRed : kGreen);
    return img;
}

void draw(Image& canvas, const Image& source, const Transform& t)
{
    compositeLayer(canvas.data.data(), canvas.width * 4, canvas.width, canvas.height, source.view(), t);
}

void expectColor(const Image& img, int x, int y, Color expected, int tolerance = 0)
{
    const Color c = img.at(x, y);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(c[i], expected[i], tolerance) << "pixel (" << x << ", " << y << ") canale " << i;
}

}  // namespace

TEST(Compositor, IdentityCopiesTheFrameExactly)
{
    Image src = halves(64, 36);
    src.set(5, 7, kBlue);
    Image canvas(64, 36, kWhite);
    draw(canvas, src, {});
    for (int y = 0; y < 36; ++y)
        for (int x = 0; x < 64; ++x)
            ASSERT_EQ(canvas.at(x, y), src.at(x, y)) << x << "," << y;
}

TEST(Compositor, SmallerFileIsFittedWithBlackBandsLeftAsTheyWere)
{
    // 50x50 in un fotogramma 100x50: scala 1, centrato, e ai lati resta il fondo.
    const Image src = halves(50, 50);
    Image canvas(100, 50, kBlue);
    draw(canvas, src, {});
    expectColor(canvas, 10, 25, kBlue);   // banda a sinistra
    expectColor(canvas, 30, 25, kRed);    // 25..50 rosso
    expectColor(canvas, 60, 25, kGreen);  // 50..75 verde
    expectColor(canvas, 90, 25, kBlue);   // banda a destra
}

TEST(Compositor, ScaleHalfDrawsACenteredQuarterOfTheArea)
{
    const Image src(100, 100, kRed);
    Image canvas(100, 100, kBlack);
    Transform t;
    t.scaleX = 0.5;
    t.scaleY = 0.5;
    draw(canvas, src, t);
    expectColor(canvas, 50, 50, kRed);
    expectColor(canvas, 26, 26, kRed);
    expectColor(canvas, 73, 73, kRed);
    expectColor(canvas, 10, 10, kBlack);
    expectColor(canvas, 90, 50, kBlack);
}

TEST(Compositor, PositionMovesByWholePixelsWithSharpEdges)
{
    const Image src(20, 20, kGreen);
    Image canvas(100, 100, kBlack);
    Transform t;
    t.scaleX = 0.2;  // 100 -> 20 pixel (adattato: scala 5 per 0.2 = 1)
    t.scaleY = 0.2;
    t.posX = 30;
    t.posY = -20;
    draw(canvas, src, t);
    // Centro a (80, 30): il quadrato va da (70,20) a (90,40).
    expectColor(canvas, 70, 20, kGreen);
    expectColor(canvas, 89, 39, kGreen);
    expectColor(canvas, 69, 30, kBlack);
    expectColor(canvas, 90, 30, kBlack);
    expectColor(canvas, 80, 19, kBlack);
    expectColor(canvas, 80, 40, kBlack);
}

TEST(Compositor, HalfPixelOffsetGivesSoftEdgeCoverage)
{
    const Image src(20, 20, kWhite);
    Image canvas(100, 100, kBlack);
    Transform t;
    t.scaleX = 0.2;
    t.scaleY = 0.2;
    t.posX = 0.5;
    draw(canvas, src, t);
    // Il quadrato va da x=40.5 a 60.5: i pixel 40 e 60 sono coperti a metà.
    expectColor(canvas, 40, 50, {128, 128, 128}, 2);
    expectColor(canvas, 60, 50, {128, 128, 128}, 2);
    expectColor(canvas, 41, 50, kWhite);
    expectColor(canvas, 39, 50, kBlack);
}

TEST(Compositor, CropShowsOnlyTheKeptPartInItsOwnPlace)
{
    const Image src = halves(100, 100);
    Image canvas(100, 100, kBlue);
    Transform t;
    t.cropLeft = 0.5;  // via la metà rossa
    draw(canvas, src, t);
    expectColor(canvas, 25, 50, kBlue);   // il ritaglio non sposta: lì resta il fondo
    expectColor(canvas, 75, 50, kGreen);
    // Ritaglio in alto: la parte alta resta com'era.
    Image canvas2(100, 100, kBlue);
    Transform t2;
    t2.cropTop = 0.25;
    draw(canvas2, src, t2);
    expectColor(canvas2, 10, 10, kBlue);
    expectColor(canvas2, 10, 30, kRed);
}

TEST(Compositor, CropDoesNotBleedColorFromTheCroppedSide)
{
    // Il confine rosso/verde è a x=50: ritagliando dal confine il bordo del verde non deve prendere il rosso.
    const Image src = halves(100, 100);
    Image canvas(100, 100, kBlack);
    Transform t;
    t.cropLeft = 0.5;
    draw(canvas, src, t);
    for (int y = 0; y < 100; ++y)
        expectColor(canvas, 50, y, kGreen);
}

TEST(Compositor, FlipMirrorsTheImage)
{
    const Image src = halves(100, 100);
    Image canvas(100, 100, kBlack);
    Transform t;
    t.flipHorizontal = true;
    draw(canvas, src, t);
    expectColor(canvas, 10, 50, kGreen);
    expectColor(canvas, 90, 50, kRed);
}

TEST(Compositor, Rotation90MovesCornersClockwise)
{
    // Quadrato 100x100: in alto a sinistra rosso, il resto verde. Ruotato di 90° orari il rosso va in alto a destra.
    Image src(100, 100, kGreen);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x)
            src.set(x, y, kRed);
    Image canvas(100, 100, kBlack);
    Transform t;
    t.rotation = 90.0;
    draw(canvas, src, t);
    expectColor(canvas, 90, 10, kRed, 3);
    expectColor(canvas, 10, 10, kGreen, 3);
    expectColor(canvas, 10, 90, kGreen, 3);
    expectColor(canvas, 50, 50, kGreen, 3);
}

TEST(Compositor, RotationLeavesTheCornersOfTheFrameUntouched)
{
    const Image src(100, 100, kRed);
    Image canvas(100, 100, kBlue);
    Transform t;
    t.rotation = 45.0;
    draw(canvas, src, t);
    expectColor(canvas, 50, 50, kRed);
    expectColor(canvas, 1, 1, kBlue);   // gli angoli restano il fondo
    expectColor(canvas, 98, 98, kBlue);
}

TEST(Compositor, OpacityBlendsWithWhatIsBelow)
{
    const Image src(40, 40, kRed);
    Image canvas(40, 40, kWhite);
    Transform t;
    t.opacity = 0.5;
    draw(canvas, src, t);
    expectColor(canvas, 20, 20, {128, 128, 255}, 1);
    Image none(40, 40, kWhite);
    t.opacity = 0.0;
    draw(none, src, t);
    expectColor(none, 20, 20, kWhite);
}

TEST(Compositor, LayersStackFromBottomToTop)
{
    const Image low(100, 100, kBlue);
    const Image high(100, 100, kRed);
    Image canvas(100, 100, kBlack);
    draw(canvas, low, {});
    Transform small;
    small.scaleX = 0.5;
    small.scaleY = 0.5;
    draw(canvas, high, small);
    expectColor(canvas, 50, 50, kRed);
    expectColor(canvas, 5, 5, kBlue);  // dove il livello alto non arriva si vede quello sotto
}

TEST(Compositor, BigReductionAveragesInsteadOfAliasing)
{
    // Scacchiera a pixel alterni, ridotta a un quarto: grigio medio, senza il disegno a scacchi.
    Image src(128, 128);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
            src.set(x, y, (x + y) % 2 == 0 ? kWhite : kBlack);
    Image canvas(128, 128, kBlue);
    Transform t;
    t.scaleX = 0.25;
    t.scaleY = 0.25;
    draw(canvas, src, t);
    for (int y = 56; y < 72; ++y)
        for (int x = 56; x < 72; ++x)
            expectColor(canvas, x, y, {128, 128, 128}, 2);
}

TEST(Compositor, ProxyOfDifferentSizeGivesTheSameShape)
{
    // Lo stesso contenuto a metà risoluzione (come un proxy) occupa la stessa area sullo schermo.
    const Image big = halves(200, 100);
    const Image small = halves(100, 50);
    Image a(200, 100, kBlack);
    Image b(200, 100, kBlack);
    Transform t;
    t.scaleX = 0.5;
    t.scaleY = 0.5;
    t.posX = 20;
    draw(a, big, t);
    draw(b, small, t);
    for (int y = 0; y < 100; y += 7)
        for (int x = 0; x < 200; x += 5)
            expectColor(a, x, y, b.at(x, y), 2);
}

TEST(Compositor, NothingToDoCases)
{
    Image canvas(10, 10, kWhite);
    compositeLayer(canvas.data.data(), 40, 10, 10, BgraImage{}, {});
    compositeLayer(nullptr, 40, 10, 10, Image(4, 4).view(), {});
    Transform far;
    far.posX = 5000;
    draw(canvas, Image(4, 4, kRed), far);  // tutto fuori dallo schermo
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            ASSERT_EQ(canvas.at(x, y), kWhite);
}
