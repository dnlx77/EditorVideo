#include "EditorCore/CutSegment.h"

#include <gtest/gtest.h>

using editor::core::CutSegment;

namespace {

// File da 10 s con fotogrammi chiave ogni 2 s (il primo non a zero, come capita in alcuni file).
CutSegment makeSegment()
{
    CutSegment segment;
    segment.reset({0.04, 2.0, 4.0, 6.0, 8.0}, 10.0);
    return segment;
}

TEST(CutSegment, WithoutMarkersCoversWholeFile)
{
    const CutSegment segment = makeSegment();
    EXPECT_FALSE(segment.hasIn());
    EXPECT_FALSE(segment.hasOut());
    EXPECT_DOUBLE_EQ(segment.in(), 0.0);
    EXPECT_DOUBLE_EQ(segment.out(), 10.0);
    EXPECT_DOUBLE_EQ(segment.length(), 10.0);
}

TEST(CutSegment, InSnapsToKeyframeAtOrBefore)
{
    CutSegment segment = makeSegment();
    EXPECT_DOUBLE_EQ(segment.setIn(4.52), 4.0);   // in mezzo: fotogramma chiave precedente
    EXPECT_DOUBLE_EQ(segment.setIn(6.0), 6.0);    // esattamente su un fotogramma chiave
    EXPECT_DOUBLE_EQ(segment.setIn(5.9996), 6.0); // differenze di arrotondamento: resta su 6
    EXPECT_DOUBLE_EQ(segment.setIn(9.9), 8.0);    // dopo l'ultimo: l'ultimo
    EXPECT_DOUBLE_EQ(segment.setIn(0.0), 0.04);   // prima del primo: il primo
    EXPECT_TRUE(segment.hasIn());
    EXPECT_DOUBLE_EQ(segment.in(), 0.04);
}

TEST(CutSegment, InIsClampedToFile)
{
    CutSegment segment = makeSegment();
    EXPECT_DOUBLE_EQ(segment.setIn(-3.0), 0.04);
    EXPECT_DOUBLE_EQ(segment.setIn(25.0), 8.0);
}

TEST(CutSegment, OutCanBeAnyPointAfterIn)
{
    CutSegment segment = makeSegment();
    segment.setIn(4.52);  // -> 4.0
    EXPECT_TRUE(segment.setOut(7.36));
    EXPECT_DOUBLE_EQ(segment.out(), 7.36);  // OUT non viene agganciato
    EXPECT_NEAR(segment.length(), 3.36, 1e-9);
}

TEST(CutSegment, OutBeforeOrAtInIsRejected)
{
    CutSegment segment = makeSegment();
    segment.setIn(4.52);  // -> 4.0
    ASSERT_TRUE(segment.setOut(7.0));

    EXPECT_FALSE(segment.setOut(3.0));
    EXPECT_FALSE(segment.setOut(4.0));
    EXPECT_DOUBLE_EQ(segment.out(), 7.0);  // invariato

    // Senza IN, OUT deve solo stare dopo l'inizio del file.
    CutSegment noIn = makeSegment();
    EXPECT_TRUE(noIn.setOut(0.5));
    EXPECT_FALSE(noIn.setOut(0.0));
}

TEST(CutSegment, OutIsClampedToDuration)
{
    CutSegment segment = makeSegment();
    EXPECT_TRUE(segment.setOut(12.0));
    EXPECT_DOUBLE_EQ(segment.out(), 10.0);
}

TEST(CutSegment, MovingInPastOutRemovesOut)
{
    CutSegment segment = makeSegment();
    segment.setIn(2.5);  // -> 2.0
    ASSERT_TRUE(segment.setOut(5.0));
    segment.setIn(7.0);  // -> 6.0, dopo OUT
    EXPECT_FALSE(segment.hasOut());
    EXPECT_DOUBLE_EQ(segment.out(), 10.0);
    EXPECT_DOUBLE_EQ(segment.length(), 4.0);

    // Se OUT resta dopo il nuovo IN, non si tocca.
    ASSERT_TRUE(segment.setOut(9.0));
    segment.setIn(3.0);  // -> 2.0
    EXPECT_TRUE(segment.hasOut());
    EXPECT_DOUBLE_EQ(segment.out(), 9.0);
}

TEST(CutSegment, ClearMarkers)
{
    CutSegment segment = makeSegment();
    segment.setIn(4.0);
    segment.setOut(6.0);
    segment.clearIn();
    EXPECT_DOUBLE_EQ(segment.in(), 0.0);
    EXPECT_DOUBLE_EQ(segment.out(), 6.0);
    segment.clearOut();
    EXPECT_DOUBLE_EQ(segment.length(), 10.0);
}

TEST(CutSegment, NoKeyframesMeansNoSnapping)
{
    // File solo audio: si può tagliare ovunque.
    CutSegment segment;
    segment.reset({}, 30.0);
    EXPECT_DOUBLE_EQ(segment.setIn(12.345), 12.345);
    EXPECT_TRUE(segment.setOut(20.0));
    EXPECT_NEAR(segment.length(), 7.655, 1e-9);
}

TEST(CutSegment, ResetClearsMarkers)
{
    CutSegment segment = makeSegment();
    segment.setIn(4.0);
    segment.setOut(6.0);
    segment.reset({1.0, 3.0, 2.0}, 5.0);  // anche fuori ordine
    EXPECT_FALSE(segment.hasIn());
    EXPECT_FALSE(segment.hasOut());
    EXPECT_DOUBLE_EQ(segment.snapToKeyframe(2.5), 2.0);
}

}  // namespace
