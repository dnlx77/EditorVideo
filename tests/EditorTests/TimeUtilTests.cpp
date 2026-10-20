#include "EditorCore/TimeUtil.h"

#include <gtest/gtest.h>

using namespace editor::core;

TEST(TimeUtil, SnapsToKeyframeAtOrBefore)
{
    const std::vector<double> keyframes{0.0, 1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(snapToKeyframe(keyframes, 1.3), 1.0);
    EXPECT_DOUBLE_EQ(snapToKeyframe(keyframes, 2.0), 2.0);
    EXPECT_DOUBLE_EQ(snapToKeyframe(keyframes, 1.9995), 2.0);  // entro la tolleranza: è già il fotogramma chiave
    EXPECT_DOUBLE_EQ(snapToKeyframe(keyframes, 9.0), 3.0);
}

TEST(TimeUtil, SnapBeforeFirstKeyframeOrWithoutKeyframes)
{
    EXPECT_DOUBLE_EQ(snapToKeyframe({0.5, 1.5}, 0.2), 0.5);
    EXPECT_DOUBLE_EQ(snapToKeyframe({}, 1.234), 1.234);
}

TEST(TimeUtil, MergeRangesSortsAndMerges)
{
    // In ordine qualunque: si ordinano; sovrapposti o che si toccano (anche entro la tolleranza): uno solo.
    const std::vector<TimeRange> merged =
        mergeRanges({{8.0, 9.0}, {2.5, 4.5}, {0.0, 1.0}, {2.0, 3.0}, {4.5005, 6.0}, {3.0, 3.5}});
    EXPECT_EQ(merged, (std::vector<TimeRange>{{0.0, 1.0}, {2.0, 6.0}, {8.0, 9.0}}));
}

TEST(TimeUtil, MergeRangesDropsEmptyAndInverted)
{
    EXPECT_TRUE(mergeRanges({{5.0, 5.0}, {5.0, 3.0}, {4.0, 4.0005}}).empty());
    EXPECT_EQ(mergeRanges({{1.0, 1.0}, {2.0, 3.0}}), (std::vector<TimeRange>{{2.0, 3.0}}));
    EXPECT_TRUE(mergeRanges({}).empty());
}

TEST(TimeUtil, ClampsIntoFile)
{
    EXPECT_DOUBLE_EQ(clampTo(-1.0, 10.0), 0.0);
    EXPECT_DOUBLE_EQ(clampTo(4.0, 10.0), 4.0);
    EXPECT_DOUBLE_EQ(clampTo(12.0, 10.0), 10.0);
    // Durata sconosciuta: solo niente tempi negativi.
    EXPECT_DOUBLE_EQ(clampTo(-1.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(clampTo(12.0, 0.0), 12.0);
}
