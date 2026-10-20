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

TEST(TimeUtil, ClampsIntoFile)
{
    EXPECT_DOUBLE_EQ(clampTo(-1.0, 10.0), 0.0);
    EXPECT_DOUBLE_EQ(clampTo(4.0, 10.0), 4.0);
    EXPECT_DOUBLE_EQ(clampTo(12.0, 10.0), 10.0);
    // Durata sconosciuta: solo niente tempi negativi.
    EXPECT_DOUBLE_EQ(clampTo(-1.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(clampTo(12.0, 0.0), 12.0);
}
