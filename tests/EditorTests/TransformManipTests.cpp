#include "EditorCore/TransformManip.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace editor::core;

namespace {

constexpr ManipSize kSize{1920, 1080, 1920, 1080};
constexpr ManipSize kWide{1280, 720, 1920, 1080};  // un file più piccolo, stesse proporzioni
constexpr ManipSize kSquare{1000, 1000, 1920, 1080};  // un file quadrato: bande ai lati

Point2 corner(const Transform& t, const ManipSize& s, int index)
{
    return computeGeometry(t, s.srcW, s.srcH, s.projW, s.projH).corners[static_cast<std::size_t>(index)];
}

void expectNear(Point2 p, Point2 q, double tolerance = 1e-6)
{
    EXPECT_NEAR(p.x, q.x, tolerance);
    EXPECT_NEAR(p.y, q.y, tolerance);
}

}  // namespace

TEST(Manip, HitTestFindsHandlesInsideAndOutside)
{
    Transform t;
    t.scaleX = t.scaleY = 0.5;  // il riquadro sta fra (480,270) e (1440,810)
    EXPECT_EQ(manipHitTest(t, kSize, {960, 540}, 0.5).part, ManipPart::Move);
    const ManipHit c = manipHitTest(t, kSize, {1440, 810}, 0.5);
    EXPECT_EQ(c.part, ManipPart::Corner);
    EXPECT_EQ(c.corner, 2);
    const Point2 handle = manipRotationHandle(t, kSize, 0.5);
    EXPECT_NEAR(handle.x, 960.0, 1e-9);
    EXPECT_NEAR(handle.y, 270.0 - 24.0 / 0.5, 1e-9);  // sopra il lato alto, a 24 px dello schermo
    EXPECT_EQ(manipHitTest(t, kSize, handle, 0.5).part, ManipPart::Rotate);
    EXPECT_EQ(manipHitTest(t, kSize, {100, 100}, 0.5).part, ManipPart::None);
    // La presa è in pixel dello schermo: a 1 px = 1 unità servono pochi pixel, a 0,1 ce ne vogliono molti di più.
    EXPECT_EQ(manipHitTest(t, kSize, {1440 + 20, 810 + 20}, 1.0).part, ManipPart::None);
    EXPECT_EQ(manipHitTest(t, kSize, {1440 + 20, 810 + 20}, 0.2).part, ManipPart::Corner);
}

TEST(Manip, MoveFollowsTheMouse)
{
    Transform t;
    t.posX = 10;
    const Transform moved = manipMove(t, {100, 100}, {160, 70});
    EXPECT_NEAR(moved.posX, 70.0, 1e-9);
    EXPECT_NEAR(moved.posY, -30.0, 1e-9);
}

TEST(Manip, ScaleKeepsTheOppositeCornerAndFollowsTheMouse)
{
    for (const ManipSize& size : {kSize, kWide, kSquare}) {
        for (int index = 0; index < 4; ++index) {
            SCOPED_TRACE(index);
            Transform t;
            const Point2 anchor = corner(t, size, (index + 2) % 4);
            const Point2 start = corner(t, size, index);
            // Si trascina l'angolo verso il centro: 70% della distanza dall'ancora, sulla diagonale.
            const Point2 to{anchor.x + (start.x - anchor.x) * 0.7, anchor.y + (start.y - anchor.y) * 0.7};
            const Transform result = manipScale(t, size, index, to, false, false);
            EXPECT_NEAR(result.scaleX, 0.7, 1e-9);
            EXPECT_NEAR(result.scaleY, 0.7, 1e-9);
            expectNear(corner(result, size, (index + 2) % 4), anchor);
            expectNear(corner(result, size, index), to);
        }
    }
}

TEST(Manip, ScaleWorksOnRotatedFlippedCroppedPieces)
{
    Transform t;
    t.scaleX = 1.3;
    t.scaleY = 0.8;
    t.rotation = 37.0;
    t.posX = 120;
    t.posY = -40;
    t.cropLeft = 0.1;
    t.cropBottom = 0.2;
    t.flipHorizontal = true;
    for (int index = 0; index < 4; ++index) {
        SCOPED_TRACE(index);
        const Point2 anchor = corner(t, kWide, (index + 2) % 4);
        const Point2 start = corner(t, kWide, index);
        // Il mouse dove sta già l'angolo: niente cambia.
        expectNear(corner(manipScale(t, kWide, index, start, false, false), kWide, index), start);
        // Più in là, a mano libera: l'angolo va sotto il mouse e l'opposto resta fermo.
        const Point2 to{start.x + 30, start.y - 20};
        const Transform free = manipScale(t, kWide, index, to, true, false);
        expectNear(corner(free, kWide, (index + 2) % 4), anchor);
        expectNear(corner(free, kWide, index), to);
        // Con le proporzioni: l'opposto resta fermo e il rapporto fra larghezza e altezza è quello di prima.
        const Transform kept = manipScale(t, kWide, index, to, false, false);
        expectNear(corner(kept, kWide, (index + 2) % 4), anchor);
        EXPECT_NEAR(kept.scaleX / kept.scaleY, t.scaleX / t.scaleY, 1e-9);
    }
}

TEST(Manip, FreeAspectChangesOnlyOneAxisAlongTheEdge)
{
    Transform t;
    // Si porta l'angolo basso-destra a metà larghezza, alla stessa altezza: solo la larghezza cambia.
    const Transform result = manipScale(t, kSize, 2, {960, 1080}, true, false);
    EXPECT_NEAR(result.scaleX, 0.5, 1e-9);
    EXPECT_NEAR(result.scaleY, 1.0, 1e-9);
    expectNear(corner(result, kSize, 0), corner(t, kSize, 0));
}

TEST(Manip, FromCenterKeepsTheCenter)
{
    Transform t;
    t.posX = 50;
    const Point2 center{960 + 50, 540};
    const Transform result = manipScale(t, kSize, 2, {center.x + 0.25 * 960, center.y + 0.25 * 540}, false, true);
    EXPECT_NEAR(result.scaleX, 0.25, 1e-9);
    EXPECT_NEAR(result.posX, 50.0, 1e-9);
    EXPECT_NEAR(result.posY, 0.0, 1e-9);
}

TEST(Manip, ScaleStaysInsideTheLimits)
{
    Transform t;
    // Oltre l'ancora (mouse dall'altra parte): scala minima, non negativa.
    const Transform crossed = manipScale(t, kSize, 2, {-500, -500}, false, false);
    EXPECT_NEAR(crossed.scaleX, kMinScale, 1e-12);
    EXPECT_GT(crossed.scaleY, 0.0);
    const Transform huge = manipScale(t, kSize, 2, {1e9, 1e9}, false, false);
    EXPECT_NEAR(huge.scaleX, kMaxScale, 1e-9);
}

TEST(Manip, RotateAddsTheAngleAroundTheCenterAndSnaps)
{
    Transform t;
    t.posX = 100;
    const Point2 pivot{1060, 540};
    const Transform quarter = manipRotate(t, kSize, {pivot.x + 200, pivot.y}, {pivot.x, pivot.y + 200}, false);
    EXPECT_NEAR(quarter.rotation, 90.0, 1e-9);  // verso il basso = senso orario
    Transform started;
    started.rotation = 10.0;
    const Transform snapped = manipRotate(started, kSize, {pivot.x + 200, 540}, {pivot.x + 200, 540 + 31}, true);
    EXPECT_NEAR(std::fmod(snapped.rotation, 15.0), 0.0, 1e-9);
    // Attraverso il lato sinistro (±180°): l'angolo cambia di pochi gradi, non di quasi 360.
    const Transform across = manipRotate(t, kSize, {pivot.x - 200, pivot.y + 10}, {pivot.x - 200, pivot.y - 10}, false);
    EXPECT_NEAR(across.rotation, 2.0 * std::atan2(10.0, 200.0) * 180.0 / 3.14159265358979323846, 1e-6);
}

// ---- Ritaglio

TEST(ManipCrop, HandlesSitOnTheCroppedFrameAndAreHit)
{
    Transform t;
    t.cropLeft = 0.25;
    t.cropBottom = 0.5;
    expectNear(cropHandlePoint(t, kSize, kCropLeft | kCropTop), {480, 0});
    expectNear(cropHandlePoint(t, kSize, kCropRight | kCropBottom), {1920, 540});
    expectNear(cropHandlePoint(t, kSize, kCropTop), {1200, 0});
    expectNear(cropHandlePoint(t, kSize, kCropLeft), {480, 270});
    EXPECT_EQ(cropHitTest(t, kSize, {482, 2}, 1.0), kCropLeft | kCropTop);
    EXPECT_EQ(cropHitTest(t, kSize, {700, 3}, 1.0), kCropTop);  // vicino al lato, non al centro
    EXPECT_EQ(cropHitTest(t, kSize, {1200, 300}, 1.0), 0);      // dentro: niente
}

TEST(ManipCrop, DraggingASideChangesOnlyThatCrop)
{
    Transform t;
    const Transform left = manipCrop(t, kSize, kCropLeft, {480, 700});
    EXPECT_NEAR(left.cropLeft, 0.25, 1e-9);
    EXPECT_NEAR(left.cropRight, 0.0, 1e-12);
    EXPECT_NEAR(left.cropTop, 0.0, 1e-12);
    const Transform corner = manipCrop(t, kSize, kCropRight | kCropBottom, {1440, 810});
    EXPECT_NEAR(corner.cropRight, 0.25, 1e-9);
    EXPECT_NEAR(corner.cropBottom, 0.25, 1e-9);
    // Oltre il lato opposto: resta almeno una striscia visibile.
    const Transform over = manipCrop(corner, kSize, kCropLeft, {1900, 0});
    EXPECT_NEAR(over.cropLeft, 1.0 - 0.25 - kCropMinVisible, 1e-9);
    // Fuori dal file: non va sotto zero.
    EXPECT_NEAR(manipCrop(t, kSize, kCropLeft, {-300, 0}).cropLeft, 0.0, 1e-12);
}

TEST(ManipCrop, WorksWhenScaledRotatedAndFlipped)
{
    Transform t;
    t.scaleX = t.scaleY = 0.5;
    t.posX = 100;
    t.rotation = 90.0;
    t.flipHorizontal = true;
    // Il punto in cui sta la maniglia ricavato dal ritaglio voluto: trascinandola lì, si ritrova lo stesso ritaglio.
    Transform wanted = t;
    wanted.cropLeft = 0.2;
    wanted.cropTop = 0.1;
    const Point2 target = cropHandlePoint(wanted, kSize, kCropLeft | kCropTop);
    const Transform got = manipCrop(t, kSize, kCropLeft | kCropTop, target);
    EXPECT_NEAR(got.cropLeft, 0.2, 1e-9);
    EXPECT_NEAR(got.cropTop, 0.1, 1e-9);
    // Con lo specchio il lato sinistro dello schermo resta il sinistro: l'altro ritaglio non si tocca.
    EXPECT_NEAR(got.cropRight, 0.0, 1e-12);
}

// ---- Guide magnetiche

TEST(ManipSnap, MoveSnapsEdgesAndCenterToTheFrame)
{
    Transform t;
    t.scaleX = t.scaleY = 0.5;  // 960 x 540 al centro
    t.posX = 3.0;               // il centro è a 3 px da quello del fotogramma
    SnapGuides guides;
    Transform out = snapMove(t, kSize, 1.0, &guides);
    EXPECT_NEAR(out.posX, 0.0, 1e-9);
    ASSERT_EQ(guides.vertical.size(), 1u);
    EXPECT_NEAR(guides.vertical[0], 960.0, 1e-9);
    // Il bordo sinistro del pezzo scatta sul bordo sinistro del fotogramma.
    t.posX = -480 + 4;
    out = snapMove(t, kSize, 1.0, &guides);
    EXPECT_NEAR(out.posX, -480.0, 1e-9);
    EXPECT_NEAR(guides.vertical.at(0), 0.0, 1e-9);
    // Lontano da tutto: niente scatto e niente guide.
    t.posX = 100;
    t.posY = 77;
    out = snapMove(t, kSize, 1.0, &guides);
    EXPECT_NEAR(out.posX, 100.0, 1e-12);
    EXPECT_NEAR(out.posY, 77.0, 1e-12);
    EXPECT_TRUE(guides.vertical.empty() && guides.horizontal.empty());
    // La soglia è in pixel dello schermo: a schermo ridotto (0.25 px per unità) 6 px sono 24 unità.
    t.posX = 20;
    t.posY = 0;
    EXPECT_NEAR(snapMove(t, kSize, 0.25, nullptr).posX, 0.0, 1e-9);
}

TEST(ManipSnap, RotationSnapsToRightAnglesAndScaleToOneHundred)
{
    Transform t;
    t.rotation = 89.0;
    bool snapped = false;
    EXPECT_NEAR(snapRotation(t, &snapped).rotation, 90.0, 1e-12);
    EXPECT_TRUE(snapped);
    t.rotation = -179.5;
    EXPECT_NEAR(snapRotation(t, &snapped).rotation, -180.0, 1e-12);
    t.rotation = 45.0;
    EXPECT_NEAR(snapRotation(t, &snapped).rotation, 45.0, 1e-12);
    EXPECT_FALSE(snapped);
    t.scaleX = 1.015;
    t.scaleY = 0.7;
    const Transform s = snapScale(t);
    EXPECT_NEAR(s.scaleX, 1.0, 1e-12);
    EXPECT_NEAR(s.scaleY, 0.7, 1e-12);
}
