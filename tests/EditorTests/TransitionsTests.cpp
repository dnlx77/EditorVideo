#include "EditorCore/Transitions.h"

#include <gtest/gtest.h>

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
