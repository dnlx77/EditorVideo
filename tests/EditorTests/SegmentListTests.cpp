#include "EditorCore/SegmentList.h"

#include <gtest/gtest.h>

using editor::core::SegmentList;
using editor::core::TimeRange;

namespace {

// File da 10 s con fotogrammi chiave ogni 2 s.
SegmentList makeList()
{
    SegmentList list;
    list.reset({0.0, 2.0, 4.0, 6.0, 8.0}, 10.0);
    return list;
}

std::vector<TimeRange> ranges(std::initializer_list<TimeRange> list)
{
    return list;
}

TEST(SegmentList, StartsEmpty)
{
    const SegmentList list = makeList();
    EXPECT_TRUE(list.empty());
    EXPECT_DOUBLE_EQ(list.totalLength(), 0.0);
}

TEST(SegmentList, AddSnapsStartToKeyframe)
{
    SegmentList list = makeList();
    EXPECT_TRUE(list.add(4.7, 5.5));
    EXPECT_EQ(list.ranges(), ranges({{4.0, 5.5}}));
    EXPECT_DOUBLE_EQ(list.totalLength(), 1.5);
}

TEST(SegmentList, KeepsRangesSortedWhateverTheOrderOfAdds)
{
    SegmentList list = makeList();
    list.add(8.0, 9.0);
    list.add(0.5, 1.0);  // -> 0.0
    list.add(4.0, 5.0);
    EXPECT_EQ(list.ranges(), ranges({{0.0, 1.0}, {4.0, 5.0}, {8.0, 9.0}}));
    EXPECT_DOUBLE_EQ(list.totalLength(), 3.0);
}

TEST(SegmentList, OverlappingAndAdjacentRangesAreMerged)
{
    SegmentList list = makeList();
    list.add(2.0, 3.0);
    list.add(2.5, 4.5);  // si sovrappone -> 2.0 .. 4.5
    EXPECT_EQ(list.ranges(), ranges({{2.0, 4.5}}));
    list.add(6.0, 7.0);
    list.add(4.6, 6.0);  // inizia a 4.0 (agganciato), tocca entrambi -> uno solo
    EXPECT_EQ(list.ranges(), ranges({{2.0, 7.0}}));
    list.add(3.0, 3.5);  // già dentro: niente cambia
    EXPECT_EQ(list.ranges(), ranges({{2.0, 7.0}}));
}

TEST(SegmentList, EmptyOrInvertedRangeIsIgnored)
{
    SegmentList list = makeList();
    EXPECT_FALSE(list.add(5.0, 5.0));
    EXPECT_FALSE(list.add(5.0, 3.0));
    EXPECT_FALSE(list.add(4.5, 4.0));
    EXPECT_FALSE(list.add(4.5, 4.0005));  // vuoto anche se l'aggancio lo allungherebbe (4.0 .. 4.0005)
    EXPECT_TRUE(list.empty());
}

TEST(SegmentList, RangesAreClampedToFile)
{
    SegmentList list = makeList();
    list.add(-5.0, 30.0);
    EXPECT_EQ(list.ranges(), ranges({{0.0, 10.0}}));
}

TEST(SegmentList, RemoveFromEmptyListStartsFromWholeFile)
{
    // "Togli la pubblicità": da 2 a 4 s (fine su un fotogramma chiave, quindi si toglie tutto il pezzo).
    SegmentList list = makeList();
    EXPECT_TRUE(list.remove(2.0, 4.0));
    EXPECT_EQ(list.ranges(), ranges({{0.0, 2.0}, {4.0, 10.0}}));
    EXPECT_DOUBLE_EQ(list.totalLength(), 8.0);
}

TEST(SegmentList, RemovedPartEndsAtKeyframeBefore)
{
    // Togliendo 2.5 .. 5.5 la parte dopo deve ripartire da un fotogramma chiave: 4.0 (resta 4.0 .. 5.5).
    SegmentList list = makeList();
    EXPECT_TRUE(list.remove(2.5, 5.5));
    EXPECT_EQ(list.ranges(), ranges({{0.0, 2.5}, {4.0, 10.0}}));
}

TEST(SegmentList, RemoveWithoutKeyframeInsideChangesNothing)
{
    // Fra 2.5 e 3.5 non c'è un fotogramma chiave: la parte dopo ripartirebbe da 2.0, cioè non si toglie niente.
    SegmentList list = makeList();
    EXPECT_FALSE(list.remove(2.5, 3.5));
    EXPECT_TRUE(list.empty());

    list.add(0.0, 10.0);
    EXPECT_FALSE(list.remove(2.5, 3.5));
    EXPECT_EQ(list.ranges(), ranges({{0.0, 10.0}}));
}

TEST(SegmentList, RemoveWholeRangesAndEnds)
{
    SegmentList list = makeList();
    list.add(0.0, 1.0);
    list.add(4.0, 5.0);
    list.add(8.0, 9.0);
    EXPECT_TRUE(list.remove(3.0, 6.0));  // copre tutto il tratto di mezzo
    EXPECT_EQ(list.ranges(), ranges({{0.0, 1.0}, {8.0, 9.0}}));
    EXPECT_TRUE(list.remove(0.5, 2.0));  // toglie la fine del primo
    EXPECT_EQ(list.ranges(), ranges({{0.0, 0.5}, {8.0, 9.0}}));
    EXPECT_TRUE(list.remove(0.0, 8.5));  // toglie il primo e l'inizio dell'ultimo (riparte da 8.0: niente da fare lì)
    EXPECT_EQ(list.ranges(), ranges({{8.0, 9.0}}));
}

TEST(SegmentList, RemoveOutsideRangesChangesNothing)
{
    SegmentList list = makeList();
    list.add(4.0, 5.0);
    EXPECT_FALSE(list.remove(6.0, 8.0));
    EXPECT_EQ(list.ranges(), ranges({{4.0, 5.0}}));
}

TEST(SegmentList, RemoveAtAndIndexAt)
{
    SegmentList list = makeList();
    list.add(0.0, 1.0);
    list.add(4.0, 5.0);
    EXPECT_EQ(list.indexAt(4.5), 1);
    EXPECT_EQ(list.indexAt(1.0), -1);  // la fine non fa parte del tratto
    EXPECT_EQ(list.indexAt(3.0), -1);
    EXPECT_FALSE(list.removeAt(3.0));
    EXPECT_TRUE(list.removeAt(4.5));
    EXPECT_EQ(list.ranges(), ranges({{0.0, 1.0}}));
    list.clear();
    EXPECT_TRUE(list.empty());
}

TEST(SegmentList, NoKeyframesMeansNoSnapping)
{
    SegmentList list;
    list.reset({}, 30.0);
    list.remove(10.25, 12.75);
    EXPECT_EQ(list.ranges(), ranges({{0.0, 10.25}, {12.75, 30.0}}));
}

TEST(SegmentList, ResetEmptiesList)
{
    SegmentList list = makeList();
    list.add(0.0, 1.0);
    list.reset({0.0}, 5.0);
    EXPECT_TRUE(list.empty());
    EXPECT_DOUBLE_EQ(list.duration(), 5.0);
}

}  // namespace
