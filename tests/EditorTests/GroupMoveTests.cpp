#include "EditorCore/Project.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

using Kind = ObjectRef::Kind;

struct Montage
{
    Project project;
    int a = 0;  // pezzi di 2 s con 1 s di spazio: 0 .. 2, 3 .. 5, 6 .. 8
    int b = 0;
    int c = 0;
    int music = 0;  // clip su A2, 3 .. 5 s
    int track = 0;

    Montage()
    {
        const int file = project.add("video.mp4");
        project.setAnalysis(file, {0.0}, 30.0);
        a = project.sequence().insert(-1, file, 0.0, 2.0);
        b = project.sequence().insert(-1, file, 4.0, 6.0);
        c = project.sequence().insert(-1, file, 8.0, 10.0);
        project.sequence().moveTo(b, 3.0);
        project.sequence().moveTo(c, 6.0);
        AudioClip clip;
        clip.path = "musica.mp3";
        clip.end = 2.0;
        clip.position = 3.0;
        clip.sourceDuration = 100.0;
        track = project.audioTracks().addTrack();
        music = project.audioTracks().add(track, clip);
    }

    double start(int id) const { return project.sequence().startOf(project.sequence().indexOf(id)); }
    double clipAt() const { return project.audioTracks().find(music)->position; }
};

}  // namespace

TEST(GroupMove, SinglePieceCarriesOnlyItsOwnAudio)
{
    Montage m;
    EXPECT_EQ(m.project.groupMoverCount({Kind::Piece, m.a}), 1);
    EXPECT_TRUE(m.project.moveGroup({Kind::Piece, m.a}, 0.5, false));
    EXPECT_NEAR(m.start(m.a), 0.5, 1e-6);
}

TEST(GroupMove, GroupMovesTogetherIntoFreeSpace)
{
    Montage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.b}, {Kind::AudioClip, m.music}}), 0);
    EXPECT_EQ(m.project.groupMoverCount({Kind::Piece, m.b}), 2);
    EXPECT_NEAR(m.project.clampGroupDelta({Kind::Piece, m.b}, 0.5, false), 0.5, 1e-6);
    EXPECT_TRUE(m.project.moveGroup({Kind::AudioClip, m.music}, 0.5, false));
    EXPECT_NEAR(m.start(m.b), 3.5, 1e-6);
    EXPECT_NEAR(m.clipAt(), 3.5, 1e-6);
}

TEST(GroupMove, WithoutPushStopsAgainstTheNeighbour)
{
    Montage m;
    m.project.groupObjects({{Kind::Piece, m.b}, {Kind::AudioClip, m.music}});
    // B finisce a 5, C parte a 6: al massimo 1 s.
    EXPECT_NEAR(m.project.clampGroupDelta({Kind::Piece, m.b}, 4.0, false), 1.0, 1e-6);
    EXPECT_FALSE(m.project.moveGroup({Kind::Piece, m.b}, 4.0, false));
    EXPECT_NEAR(m.start(m.b), 3.0, 1e-6);
    // All'indietro: fino alla fine di A (2): 1 s.
    EXPECT_NEAR(m.project.clampGroupDelta({Kind::Piece, m.b}, -4.0, false), -1.0, 1e-6);
}

TEST(GroupMove, PushCarriesTheFollowingGroups)
{
    Montage m;
    m.project.groupObjects({{Kind::Piece, m.b}, {Kind::AudioClip, m.music}});
    // B + musica 2 s avanti: B arriva a 5..7 e spinge C da 6 a 7.
    EXPECT_TRUE(m.project.moveGroup({Kind::Piece, m.b}, 2.0, true));
    EXPECT_NEAR(m.start(m.b), 5.0, 1e-6);
    EXPECT_NEAR(m.start(m.c), 7.0, 1e-6);
    EXPECT_NEAR(m.clipAt(), 5.0, 1e-6);
    EXPECT_NEAR(m.start(m.a), 0.0, 1e-6);
}

TEST(GroupMove, PushedPieceDragsItsGroup)
{
    Montage m;
    // La musica (3..5, su A2) è nel gruppo con C (6..8): spingere C sposta anche la musica.
    m.project.groupObjects({{Kind::AudioClip, m.music}, {Kind::Piece, m.c}});
    EXPECT_TRUE(m.project.moveGroup({Kind::Piece, m.b}, 1.5, true));  // B 4.5..6.5 urta C (6)
    EXPECT_NEAR(m.start(m.b), 4.5, 1e-6);
    EXPECT_NEAR(m.start(m.c), 6.5, 1e-6);
    EXPECT_NEAR(m.clipAt(), 3.5, 1e-6);
}

TEST(GroupMove, CannotGoBeforeZero)
{
    Montage m;
    EXPECT_NEAR(m.project.clampGroupDelta({Kind::Piece, m.a}, -1.0, false), 0.0, 1e-6);
    EXPECT_FALSE(m.project.moveGroup({Kind::Piece, m.a}, -1.0, true));
}
