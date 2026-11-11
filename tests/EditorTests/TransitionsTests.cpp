#include "EditorCore/Transitions.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace editor::core;

namespace {

// Fotogramma di prova: colore uniforme, con alfa 255.
struct Frame
{
    int width;
    int height;
    std::vector<std::uint8_t> data;

    Frame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) : width(w), height(h), data(static_cast<size_t>(w) * h * 4)
    {
        for (size_t i = 0; i < data.size(); i += 4) {
            data[i] = r;
            data[i + 1] = g;
            data[i + 2] = b;
            data[i + 3] = 255;
        }
    }
    RgbaView view() const { return {data.data(), width, height, width * 4}; }
    const std::uint8_t* px(int x, int y) const { return &data[(static_cast<size_t>(y) * width + x) * 4]; }
};

Frame run(const Transition& t, const Frame& a, const Frame& b, double progress, TransitionParams params = {})
{
    Frame out(a.width, a.height, 0, 0, 0);
    t.blend(a.view(), b.view(), progress, params, {out.data.data(), out.width * 4});
    return out;
}

bool same(const Frame& x, const Frame& y)
{
    return x.data == y.data;
}

}  // namespace

TEST(Transitions, RegistryHasUniqueIdsAndNames)
{
    const auto& all = transitionRegistry();
    ASSERT_GE(all.size(), 3u);
    std::set<std::string> ids;
    for (const Transition* t : all) {
        EXPECT_TRUE(ids.insert(t->id()).second) << t->id();
        EXPECT_NE(std::string(t->name()), "");
        EXPECT_EQ(findTransition(t->id()), t);
    }
    EXPECT_EQ(findTransition("non-esiste"), nullptr);
    ASSERT_NE(findTransition("scorrimento"), nullptr);
    EXPECT_TRUE(findTransition("scorrimento")->usesDirection());
    EXPECT_FALSE(findTransition("dissolvenza")->usesDirection());
}

TEST(Transitions, EveryEffectStartsAsFromAndEndsAsTo)
{
    const Frame a(16, 12, 200, 20, 30);
    const Frame b(16, 12, 10, 220, 90);
    for (const Transition* t : transitionRegistry()) {
        for (const auto dir : {TransitionDirection::Left, TransitionDirection::Right, TransitionDirection::Up, TransitionDirection::Down}) {
            TransitionParams p;
            p.direction = dir;
            EXPECT_TRUE(same(run(*t, a, b, 0.0, p), a)) << t->id();
            EXPECT_TRUE(same(run(*t, a, b, 1.0, p), b)) << t->id();
            EXPECT_TRUE(same(run(*t, a, b, -3.0, p), a)) << t->id();  // fuori da 0..1 si limita
            EXPECT_TRUE(same(run(*t, a, b, 7.0, p), b)) << t->id();
        }
    }
}

TEST(Transitions, CrossfadeAtHalfIsTheAverage)
{
    const Frame a(4, 4, 200, 0, 100);
    const Frame b(4, 4, 0, 100, 200);
    const Frame mid = run(*findTransition("dissolvenza"), a, b, 0.5);
    EXPECT_EQ(mid.px(1, 1)[0], 100);
    EXPECT_EQ(mid.px(1, 1)[1], 50);
    EXPECT_EQ(mid.px(1, 1)[2], 150);
    EXPECT_EQ(mid.px(1, 1)[3], 255);
    const Frame quarter = run(*findTransition("dissolvenza"), a, b, 0.25);
    EXPECT_EQ(quarter.px(0, 0)[0], 150);  // 3/4 di a
}

TEST(Transitions, DipToBlackGoesThroughBlack)
{
    const Frame a(4, 4, 200, 100, 50);
    const Frame b(4, 4, 80, 160, 240);
    const Transition& t = *findTransition("dissolvenza-nero");
    const Frame mid = run(t, a, b, 0.5);
    EXPECT_EQ(mid.px(2, 2)[0], 0);
    EXPECT_EQ(mid.px(2, 2)[1], 0);
    EXPECT_EQ(mid.px(2, 2)[2], 0);
    EXPECT_EQ(mid.px(2, 2)[3], 255);
    const Frame early = run(t, a, b, 0.25);  // a a metà luminosità
    EXPECT_EQ(early.px(0, 0)[0], 100);
    EXPECT_EQ(early.px(0, 0)[2], 25);
    const Frame late = run(t, a, b, 0.75);  // b a metà luminosità
    EXPECT_EQ(late.px(0, 0)[1], 80);
    EXPECT_EQ(late.px(0, 0)[2], 120);
}

TEST(Transitions, SlideLeftPushesTheFirstOutToTheLeft)
{
    const Frame a(20, 10, 255, 0, 0);
    const Frame b(20, 10, 0, 0, 255);
    TransitionParams p;
    p.direction = TransitionDirection::Left;
    const Frame half = run(*findTransition("scorrimento"), a, b, 0.5, p);
    for (int y : {0, 9}) {
        EXPECT_EQ(half.px(0, y)[0], 255) << "x=0";   // ancora a
        EXPECT_EQ(half.px(9, y)[0], 255) << "x=9";
        EXPECT_EQ(half.px(10, y)[2], 255) << "x=10";  // già b
        EXPECT_EQ(half.px(19, y)[2], 255) << "x=19";
    }
}

TEST(Transitions, SlideRightUpDownMirrorLeft)
{
    const Frame a(20, 10, 255, 0, 0);
    const Frame b(20, 10, 0, 0, 255);
    const Transition& t = *findTransition("scorrimento");
    TransitionParams right;
    right.direction = TransitionDirection::Right;
    const Frame r = run(t, a, b, 0.25, right);  // 5 colonne di b a sinistra
    EXPECT_EQ(r.px(4, 3)[2], 255);
    EXPECT_EQ(r.px(5, 3)[0], 255);
    EXPECT_EQ(r.px(19, 3)[0], 255);
    TransitionParams up;
    up.direction = TransitionDirection::Up;
    const Frame u = run(t, a, b, 0.4, up);  // 4 righe di b in basso
    EXPECT_EQ(u.px(7, 5)[0], 255);
    EXPECT_EQ(u.px(7, 6)[2], 255);
    EXPECT_EQ(u.px(7, 9)[2], 255);
    TransitionParams down;
    down.direction = TransitionDirection::Down;
    const Frame d = run(t, a, b, 0.4, down);  // 4 righe di b in alto
    EXPECT_EQ(d.px(7, 3)[2], 255);
    EXPECT_EQ(d.px(7, 4)[0], 255);
}

TEST(Transitions, SlideMovesTheContentNotJustColors)
{
    // a: gradiente orizzontale (x*10), b: tutto 255. Dopo uno scorrimento a sinistra di 5 colonne, la colonna 0 è
    // quella che era la 5.
    Frame a(20, 4, 0, 0, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 20; ++x)
            a.data[(static_cast<size_t>(y) * 20 + x) * 4] = static_cast<std::uint8_t>(x * 10);
    const Frame b(20, 4, 255, 255, 255);
    TransitionParams p;
    p.direction = TransitionDirection::Left;
    const Frame out = run(*findTransition("scorrimento"), a, b, 0.25, p);
    EXPECT_EQ(out.px(0, 2)[0], 50);
    EXPECT_EQ(out.px(14, 2)[0], 190);
    EXPECT_EQ(out.px(15, 2)[0], 255);
}

TEST(Transitions, OutputMayBeOneOfTheInputs)
{
    Frame a(16, 8, 255, 0, 0);
    const Frame b(16, 8, 0, 0, 255);
    const Frame expected = run(*findTransition("scorrimento"), a, b, 0.5);
    const Transition& t = *findTransition("scorrimento");
    t.blend(a.view(), b.view(), 0.5, {}, {a.data.data(), a.width * 4});
    EXPECT_TRUE(same(a, expected));
    Frame c(16, 8, 100, 100, 100);
    const Frame mixExpected = run(*findTransition("dissolvenza"), c, b, 0.5);
    findTransition("dissolvenza")->blend(c.view(), b.view(), 0.5, {}, {c.data.data(), c.width * 4});
    EXPECT_TRUE(same(c, mixExpected));
}

TEST(Transitions, RespectsStrideAndKeepsAlphaOfFrom)
{
    // Riga di 16 byte per 2 pixel utili (stride più largo della riga) e alfa diverso da 255 in `from`.
    std::vector<std::uint8_t> a(3 * 16, 0), b(3 * 16, 0), out(3 * 16, 7);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 2; ++x) {
            std::uint8_t* pa = &a[y * 16 + x * 4];
            std::uint8_t* pb = &b[y * 16 + x * 4];
            pa[0] = 100; pa[3] = 128;
            pb[0] = 200; pb[3] = 255;
        }
    findTransition("dissolvenza")->blend({a.data(), 2, 3, 16}, {b.data(), 2, 3, 16}, 0.5, {}, {out.data(), 16});
    EXPECT_EQ(out[16 + 4], 150);
    EXPECT_EQ(out[16 + 7], 128);
    EXPECT_EQ(out[16 + 8], 7);  // oltre la riga utile non si scrive
}

TEST(Transitions, DipToWhiteGoesThroughWhite)
{
    const Frame a(4, 4, 200, 100, 50);
    const Frame b(4, 4, 80, 160, 240);
    const Transition& t = *findTransition("dissolvenza-bianco");
    const Frame mid = run(t, a, b, 0.5);
    for (int c = 0; c < 3; ++c)
        EXPECT_EQ(mid.px(2, 2)[c], 255);
    EXPECT_EQ(mid.px(2, 2)[3], 255);
    const Frame early = run(t, a, b, 0.25);  // metà a, metà bianco
    EXPECT_EQ(early.px(0, 0)[0], 228);       // (200 + 255) / 2 = 227.5
    EXPECT_EQ(early.px(0, 0)[1], 178);       // 177.5
    const Frame late = run(t, a, b, 0.75);   // metà bianco, metà b
    EXPECT_EQ(late.px(0, 0)[0], 168);        // 167.5
    EXPECT_EQ(late.px(0, 0)[2], 248);        // 247.5
}

TEST(Transitions, WipeRevealsTheSecondFromTheEnteringSide)
{
    const Frame a(100, 10, 255, 0, 0);
    const Frame b(100, 10, 0, 0, 255);
    const Transition& t = *findTransition("tendina");
    EXPECT_TRUE(t.usesDirection());
    TransitionParams left;
    left.direction = TransitionDirection::Left;
    const Frame half = run(t, a, b, 0.5, left);
    EXPECT_EQ(half.px(5, 4)[0], 255);   // lontano dal bordo: ancora a
    EXPECT_EQ(half.px(94, 4)[2], 255);  // lato destro: già b
    // Il bordo è vicino alla metà, e a destra del bordo c'è solo più b.
    int firstB = -1;
    for (int x = 0; x < 100; ++x)
        if (half.px(x, 4)[2] > 128) {
            firstB = x;
            break;
        }
    EXPECT_NEAR(firstB, 50, 1);
    for (int x = 1; x < 100; ++x)
        EXPECT_GE(half.px(x, 4)[2], half.px(x - 1, 4)[2]);  // sfumatura monotona
    // Tutte le righe uguali (tendina verticale al bordo).
    for (int y = 1; y < 10; ++y)
        EXPECT_EQ(std::memcmp(half.px(0, y), half.px(0, 0), 100 * 4), 0);
    // Destra: speculare.
    TransitionParams right;
    right.direction = TransitionDirection::Right;
    const Frame r = run(t, a, b, 0.5, right);
    for (int x = 0; x < 100; ++x)
        EXPECT_EQ(r.px(x, 4)[2], half.px(99 - x, 4)[2]);
    // Su e giù: il bordo si muove in verticale.
    const Frame a2(10, 100, 255, 0, 0);
    const Frame b2(10, 100, 0, 0, 255);
    TransitionParams up;
    up.direction = TransitionDirection::Up;
    const Frame u = run(t, a2, b2, 0.25, up);
    EXPECT_EQ(u.px(3, 5)[0], 255);
    EXPECT_EQ(u.px(3, 95)[2], 255);
    TransitionParams down;
    down.direction = TransitionDirection::Down;
    const Frame d = run(t, a2, b2, 0.25, down);
    EXPECT_EQ(d.px(3, 4)[2], 255);
    EXPECT_EQ(d.px(3, 94)[0], 255);
}

TEST(Transitions, CoverKeepsTheFirstStillAndSlidesTheSecondOverIt)
{
    // a: gradiente orizzontale (x*10), b: gradiente diverso (255 - x*10).
    Frame a(20, 4, 0, 0, 0);
    Frame b(20, 4, 0, 0, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 20; ++x) {
            a.data[(static_cast<size_t>(y) * 20 + x) * 4] = static_cast<std::uint8_t>(x * 10);
            b.data[(static_cast<size_t>(y) * 20 + x) * 4] = static_cast<std::uint8_t>(255 - x * 10);
        }
    TransitionParams left;
    left.direction = TransitionDirection::Left;
    const Frame out = run(*findTransition("copertura"), a, b, 0.25, left);  // 5 colonne di b a destra
    EXPECT_EQ(out.px(0, 1)[0], 0);
    EXPECT_EQ(out.px(14, 1)[0], 140);                      // a fermo
    EXPECT_EQ(out.px(15, 1)[0], b.px(0, 1)[0]);            // il bordo sinistro di b
    EXPECT_EQ(out.px(19, 1)[0], b.px(4, 1)[0]);
    const Frame mid = run(*findTransition("copertura"), a, b, 0.5, left);
    EXPECT_EQ(mid.px(9, 1)[0], 90);
    EXPECT_EQ(mid.px(10, 1)[0], b.px(0, 1)[0]);
}

TEST(Transitions, UncoverSlidesTheFirstAwayAndShowsTheSecondStill)
{
    Frame a(20, 4, 0, 0, 0);
    Frame b(20, 4, 0, 0, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 20; ++x) {
            a.data[(static_cast<size_t>(y) * 20 + x) * 4] = static_cast<std::uint8_t>(x * 10);
            b.data[(static_cast<size_t>(y) * 20 + x) * 4] = static_cast<std::uint8_t>(255 - x * 10);
        }
    TransitionParams left;
    left.direction = TransitionDirection::Left;
    const Frame out = run(*findTransition("scopertura"), a, b, 0.25, left);  // a va a sinistra di 5 colonne
    EXPECT_EQ(out.px(0, 1)[0], a.px(5, 1)[0]);
    EXPECT_EQ(out.px(14, 1)[0], a.px(19, 1)[0]);
    EXPECT_EQ(out.px(15, 1)[0], b.px(15, 1)[0]);  // b fermo
    EXPECT_EQ(out.px(19, 1)[0], b.px(19, 1)[0]);
    TransitionParams down;
    down.direction = TransitionDirection::Down;
    const Frame a2(4, 20, 255, 0, 0);
    const Frame b2(4, 20, 0, 0, 255);
    const Frame d = run(*findTransition("scopertura"), a2, b2, 0.25, down);  // a scende: in alto compare b
    EXPECT_EQ(d.px(1, 4)[2], 255);
    EXPECT_EQ(d.px(1, 5)[0], 255);
    const Frame c = run(*findTransition("copertura"), a2, b2, 0.25, down);  // b scende sopra a: entra dall'alto
    EXPECT_EQ(c.px(1, 4)[2], 255);
    EXPECT_EQ(c.px(1, 5)[0], 255);
}

TEST(Transitions, IrisOpensFromTheCenter)
{
    const Frame a(60, 40, 255, 0, 0);
    const Frame b(60, 40, 0, 0, 255);
    const Transition& t = *findTransition("iris");
    EXPECT_FALSE(t.usesDirection());
    const Frame early = run(t, a, b, 0.2);
    EXPECT_EQ(early.px(30, 20)[2], 255);  // il centro è già b
    EXPECT_EQ(early.px(0, 0)[0], 255);    // l'angolo è ancora a
    EXPECT_EQ(early.px(59, 39)[0], 255);
    const Frame late = run(t, a, b, 0.99);
    EXPECT_EQ(late.px(30, 20)[2], 255);
    EXPECT_EQ(late.px(0, 0)[2], 255);     // quasi alla fine anche l'angolo è b
    // Il cerchio cresce: i pixel di b non diminuiscono mai con l'avanzamento.
    int previous = 0;
    for (double p = 0.0; p <= 1.0; p += 0.1) {
        const Frame f = run(t, a, b, p);
        int count = 0;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 60; ++x)
                count += f.px(x, y)[2] > 128;
        EXPECT_GE(count, previous);
        previous = count;
    }
    // Simmetrico rispetto al centro.
    const Frame mid = run(t, a, b, 0.4);
    EXPECT_EQ(std::memcmp(mid.px(10, 5), mid.px(49, 34), 4), 0);
}

TEST(Transitions, ClockSweepsClockwiseFromTwelve)
{
    const Frame a(40, 40, 255, 0, 0);
    const Frame b(40, 40, 0, 0, 255);
    const Transition& t = *findTransition("orologio");
    const Frame quarter = run(t, a, b, 0.25);  // da ore 12 a ore 3 già b
    EXPECT_EQ(quarter.px(30, 10)[2], 255);     // in alto a destra
    EXPECT_EQ(quarter.px(30, 30)[0], 255);     // in basso a destra ancora a
    EXPECT_EQ(quarter.px(10, 10)[0], 255);
    const Frame half = run(t, a, b, 0.5);
    EXPECT_EQ(half.px(30, 30)[2], 255);        // metà destra b
    EXPECT_EQ(half.px(10, 30)[0], 255);        // metà sinistra a
    const Frame threeQuarters = run(t, a, b, 0.75);
    EXPECT_EQ(threeQuarters.px(10, 30)[2], 255);
    EXPECT_EQ(threeQuarters.px(10, 10)[0], 255);  // manca solo in alto a sinistra
}

TEST(Transitions, ZoomEnlargesTheFirstAroundTheCenterWhileFading)
{
    // a: 64x64 con un quadrato bianco (rosso+verde+blu = 255) al centro 16..47 su fondo nero; b: tutto nero.
    Frame a(64, 64, 0, 0, 0);
    for (int y = 16; y < 48; ++y)
        for (int x = 16; x < 48; ++x)
            a.data[(static_cast<size_t>(y) * 64 + x) * 4] = a.data[(static_cast<size_t>(y) * 64 + x) * 4 + 1] = a.data[(static_cast<size_t>(y) * 64 + x) * 4 + 2] = 255;
    const Frame b(64, 64, 0, 0, 0);
    const Transition& t = *findTransition("zoom");
    const Frame half = run(t, a, b, 0.5);  // ingrandimento 1.5x: il quadrato occupa 8..55, luminosità a metà
    EXPECT_NEAR(half.px(32, 32)[0], 128, 2);
    EXPECT_NEAR(half.px(10, 32)[0], 128, 2);  // fuori dall'originale, dentro l'ingrandito
    EXPECT_EQ(half.px(2, 32)[0], 0);
    EXPECT_EQ(half.px(32, 2)[0], 0);
    // Simmetrico rispetto al centro.
    EXPECT_NEAR(half.px(10, 32)[0], half.px(53, 32)[0], 2);
    EXPECT_NEAR(half.px(32, 10)[0], half.px(32, 53)[0], 2);
}

TEST(Transitions, BlindsRevealInEveryStripe)
{
    const Frame a(80, 8, 255, 0, 0);
    const Frame b(80, 8, 0, 0, 255);
    const Transition& t = *findTransition("persiana");
    EXPECT_TRUE(t.usesDirection());
    TransitionParams left;
    left.direction = TransitionDirection::Left;
    ASSERT_EQ(blindsBandWidth(80), 10);
    const Frame half = run(t, a, b, 0.5, left);  // in ogni striscia di 10: 5 colonne di b, sul lato destro
    for (int stripe = 0; stripe < 8; ++stripe) {
        EXPECT_EQ(half.px(stripe * 10 + 4, 3)[0], 255) << stripe;
        EXPECT_EQ(half.px(stripe * 10 + 5, 3)[2], 255) << stripe;
        EXPECT_EQ(half.px(stripe * 10 + 9, 3)[2], 255) << stripe;
    }
    TransitionParams right;
    right.direction = TransitionDirection::Right;
    const Frame r = run(t, a, b, 0.5, right);  // lato sinistro di ogni striscia
    EXPECT_EQ(r.px(0, 3)[2], 255);
    EXPECT_EQ(r.px(4, 3)[2], 255);
    EXPECT_EQ(r.px(5, 3)[0], 255);
    EXPECT_EQ(r.px(9, 3)[0], 255);
    // Su/giù: strisce orizzontali.
    const Frame a2(8, 80, 255, 0, 0);
    const Frame b2(8, 80, 0, 0, 255);
    TransitionParams down;
    down.direction = TransitionDirection::Down;
    const Frame d = run(t, a2, b2, 0.5, down);
    EXPECT_EQ(d.px(3, 4)[2], 255);
    EXPECT_EQ(d.px(3, 5)[0], 255);
    EXPECT_EQ(d.px(3, 14)[2], 255);
}

TEST(Transitions, MosaicPixelatesFirstThenRevealsTheSecond)
{
    Frame a(64, 64, 0, 0, 0);
    Frame b(64, 64, 0, 0, 0);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            a.data[(static_cast<size_t>(y) * 64 + x) * 4] = static_cast<std::uint8_t>(x * 4);
            b.data[(static_cast<size_t>(y) * 64 + x) * 4 + 2] = static_cast<std::uint8_t>(y * 4);
        }
    const Transition& t = *findTransition("mosaico");
    const int block = mosaicBlockSize(0.25, 64, 64);
    EXPECT_GT(block, 1);
    EXPECT_EQ(mosaicBlockSize(0.0, 64, 64), 1);
    EXPECT_EQ(mosaicBlockSize(1.0, 64, 64), 1);
    EXPECT_EQ(mosaicBlockSize(0.5, 64, 64), 8);
    const Frame early = run(t, a, b, 0.25);
    // Dentro un blocco tutti i pixel sono uguali e vengono da a; il rosso è quello del centro del blocco.
    EXPECT_EQ(std::memcmp(early.px(0, 0), early.px(block - 1, block - 1), 3), 0);
    EXPECT_EQ(early.px(0, 0)[0], a.px(block / 2, block / 2)[0]);
    EXPECT_EQ(early.px(0, 0)[2], 0);
    const Frame late = run(t, a, b, 0.75);
    EXPECT_EQ(late.px(5, 0)[0], 0);  // b non ha rosso
    EXPECT_EQ(std::memcmp(late.px(0, 0), late.px(block - 1, block - 1), 3), 0);
    EXPECT_EQ(late.px(0, 0)[2], b.px(0, block / 2)[2]);
}
