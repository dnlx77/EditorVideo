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

TEST(Project, MovingAPieceDragsTheAudioOfTracksWithTheMagnet)
{
    Project project;
    const int clip = project.add("video.mp4");
    project.setAnalysis(clip, {0.0}, 10.0);
    const int piece = project.sequence().insert(-1, clip, 0.0, 4.0);  // 0 .. 4
    AudioTracks& audio = project.audioTracks();
    const int withMagnet = audio.addTrack();
    const int without = audio.addTrack();
    audio.setMagnet(withMagnet, true);
    const auto clipAt = [](const char* path, double position, double length) {
        AudioClip c;
        c.path = path;
        c.end = length;
        c.position = position;
        c.sourceDuration = 60.0;
        return c;
    };
    const int under = audio.add(withMagnet, clipAt("musica.mp3", 1.0, 2.0));    // 1 .. 3, sotto il pezzo
    const int outside = audio.add(withMagnet, clipAt("fuori.mp3", 6.0, 1.0));   // 6 .. 7, fuori
    const int other = audio.add(without, clipAt("altra.mp3", 1.0, 2.0));       // sotto, ma senza calamita

    bool moved = false;
    bool stayed = false;
    ASSERT_TRUE(project.moveFragment(piece, 2.0, &moved, &stayed));
    EXPECT_TRUE(moved);
    EXPECT_FALSE(stayed);
    EXPECT_DOUBLE_EQ(project.sequence().starts()[0], 2.0);
    EXPECT_DOUBLE_EQ(audio.find(under)->position, 3.0);   // è andata con il pezzo
    EXPECT_DOUBLE_EQ(audio.find(outside)->position, 6.0);  // non toccava il pezzo
    EXPECT_DOUBLE_EQ(audio.find(other)->position, 1.0);    // traccia senza calamita

    // Indietro di 1 s: torna a 2 s, quella dopo non si urta.
    ASSERT_TRUE(project.moveFragment(piece, 1.0, &moved, &stayed));
    EXPECT_DOUBLE_EQ(audio.find(under)->position, 2.0);

    // Spostamento in cui la clip urterebbe un'altra della sua traccia (a 4 .. 6): si sposta solo il pezzo.
    audio.add(withMagnet, clipAt("blocco.mp3", 4.5, 1.0));
    ASSERT_TRUE(project.moveFragment(piece, 3.0, &moved, &stayed));
    EXPECT_FALSE(moved);
    EXPECT_TRUE(stayed);
    EXPECT_DOUBLE_EQ(project.sequence().starts()[0], 3.0);
    EXPECT_DOUBLE_EQ(audio.find(under)->position, 2.0);

    // Pezzo che non si può spostare: niente cambia.
    EXPECT_FALSE(project.moveFragment(999, 1.0));
}
