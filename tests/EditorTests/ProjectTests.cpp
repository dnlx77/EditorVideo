#include "EditorCore/Project.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

TEST(Project, ClipsKeepTheirOrderAndIds)
{
    Project project;
    const int a = project.add("a.mp4");
    const int b = project.add("b.mp4");
    const int c = project.add("c.mp4");
    EXPECT_NE(a, b);
    ASSERT_EQ(project.clips().size(), 3u);
    EXPECT_EQ(project.indexOf(c), 2);

    project.move(c, 0);  // c in testa
    EXPECT_EQ(project.clips()[0].path, "c.mp4");
    EXPECT_EQ(project.clips()[1].path, "a.mp4");
    project.move(c, 99);  // oltre la fine: in fondo
    EXPECT_EQ(project.clips()[2].path, "c.mp4");

    project.remove(a);
    ASSERT_EQ(project.clips().size(), 2u);
    EXPECT_EQ(project.indexOf(a), -1);
    EXPECT_EQ(project.find(b)->path, "b.mp4");
}

TEST(Project, ClipWithoutSegmentsExportsWholeFile)
{
    Project project;
    const int id = project.add("a.mp4");
    EXPECT_TRUE(project.exportPlan().empty());  // non ancora analizzato
    EXPECT_FALSE(project.allAnalyzed());

    project.setAnalysis(id, {0.0, 2.0, 4.0}, 6.0);
    EXPECT_TRUE(project.allAnalyzed());
    const auto plan = project.exportPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].path, "a.mp4");
    EXPECT_EQ(plan[0].ranges, (std::vector<TimeRange>{{0.0, 6.0}}));
    EXPECT_DOUBLE_EQ(project.totalLength(), 6.0);
}

TEST(Project, PlanFollowsClipOrderAndSegments)
{
    Project project;
    const int a = project.add("a.mp4");
    const int b = project.add("b.mp4");
    project.setAnalysis(a, {0.0, 2.0}, 4.0);
    project.setAnalysis(b, {0.0, 1.0, 2.0}, 3.0);
    project.find(b)->segments.add(1.0, 1.5);
    project.find(b)->segments.add(2.0, 3.0);

    auto plan = project.exportPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0].ranges, (std::vector<TimeRange>{{0.0, 4.0}}));
    EXPECT_EQ(plan[1].ranges, (std::vector<TimeRange>{{1.0, 1.5}, {2.0, 3.0}}));
    EXPECT_DOUBLE_EQ(project.totalLength(), 5.5);

    project.move(b, 0);
    plan = project.exportPlan();
    EXPECT_EQ(plan[0].path, "b.mp4");
    EXPECT_EQ(plan[1].path, "a.mp4");
}

TEST(Project, WholeFileStartsFromFirstKeyframe)
{
    Project project;
    const int id = project.add("a.mp4");
    project.setAnalysis(id, {0.5, 2.5}, 4.0);
    EXPECT_EQ(project.exportPlan()[0].ranges, (std::vector<TimeRange>{{0.5, 4.0}}));
}

}  // namespace
