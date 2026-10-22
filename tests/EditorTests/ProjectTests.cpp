#include "EditorCore/Project.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

TEST(Project, ClipsAndAnalysis)
{
    Project project;
    const int a = project.add("a.mp4");
    const int b = project.add("b.mp4");
    EXPECT_NE(a, b);
    EXPECT_FALSE(project.allAnalyzed());
    project.setAnalysis(a, {2.0, 0.0}, 6.0);  // anche fuori ordine
    project.setAnalysis(b, {}, 3.0);
    EXPECT_TRUE(project.allAnalyzed());
    EXPECT_DOUBLE_EQ(project.find(a)->snapToKeyframe(1.5), 0.0);
    EXPECT_DOUBLE_EQ(project.find(a)->snapToKeyframe(2.0), 2.0);
    EXPECT_DOUBLE_EQ(project.find(b)->snapToKeyframe(1.5), 1.5);  // senza fotogrammi chiave non si aggancia
    EXPECT_EQ(project.indexOf(b), 1);
}

TEST(Project, ExportPlanFollowsTheSequence)
{
    Project project;
    const int a = project.add("a.mp4");
    const int b = project.add("b.mp4");
    project.setAnalysis(a, {0.0, 2.0, 4.0}, 6.0);
    project.setAnalysis(b, {0.0, 1.0, 2.0}, 3.0);
    EXPECT_TRUE(project.exportPlan().empty());

    Sequence& s = project.sequence();
    // Taglio preciso: i tempi restano quelli scelti, anche fra due fotogrammi chiave.
    s.insert(-1, b, 1.5, 2.5);
    s.insert(-1, a, 4.0, 6.0);
    s.insert(-1, a, 0.5, 1.0);  // stesso file del precedente: stesso blocco
    s.insert(-1, b, 0.0, 1.0);

    const auto plan = project.exportPlan();
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[0].path, "b.mp4");
    EXPECT_EQ(plan[0].ranges, (std::vector<TimeRange>{{1.5, 2.5}}));
    EXPECT_EQ(plan[0].keyframes, (std::vector<double>{0.0, 1.0, 2.0}));  // per il taglio preciso
    EXPECT_EQ(plan[1].path, "a.mp4");
    EXPECT_EQ(plan[1].ranges, (std::vector<TimeRange>{{4.0, 6.0}, {0.5, 1.0}}));
    EXPECT_EQ(plan[2].path, "b.mp4");
    EXPECT_DOUBLE_EQ(s.totalLength(), 4.5);
}

TEST(Project, RemovingClipRemovesItsPieces)
{
    Project project;
    const int a = project.add("a.mp4");
    const int b = project.add("b.mp4");
    project.setAnalysis(a, {0.0}, 6.0);
    project.setAnalysis(b, {0.0}, 3.0);
    project.sequence().insert(-1, a, 0.0, 1.0);
    project.sequence().insert(-1, b, 0.0, 1.0);
    project.remove(a);
    ASSERT_EQ(project.sequence().fragments().size(), 1u);
    EXPECT_EQ(project.sequence().fragments()[0].clipId, b);
    EXPECT_EQ(project.find(a), nullptr);
    project.clear();
    EXPECT_TRUE(project.sequence().empty());
}

}  // namespace
