#include "EditorCore/Project.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <vector>

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

TEST(GroupSplitAndRemove, SplitDividesAllCrossedMembersAndMakesANewGroup)
{
    Montage m;  // A 0..2, B 3..5, C 6..8; musica su A2 3..5
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.b}, {Kind::AudioClip, m.music}}), 0);
    const std::vector<ObjectRef> parts = m.project.splitGroup({Kind::Piece, m.b}, 4.0);
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(m.project.sequence().fragments().size(), 4u);
    // Le seconde parti sono insieme e separate dalle prime.
    EXPECT_EQ(m.project.groupMoverCount(parts[0]), 2);
    EXPECT_EQ(m.project.groupMoverCount({Kind::Piece, m.b}), 2);
    const auto firstGroup = m.project.groupMembers({Kind::Piece, m.b});
    EXPECT_EQ(std::find(firstGroup.begin(), firstGroup.end(), parts[0]), firstGroup.end());
    // Fuori da ogni oggetto del gruppo non si divide.
    EXPECT_TRUE(m.project.splitGroup({Kind::Piece, m.b}, 7.0).empty());
}

TEST(GroupSplitAndRemove, RemoveObjectsKeepsTheRestOfTheGroup)
{
    Montage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.b}, {Kind::Piece, m.c}, {Kind::AudioClip, m.music}}), 0);
    EXPECT_EQ(m.project.removeObjects({{Kind::AudioClip, m.music}, {Kind::Piece, m.c}}), 2);
    EXPECT_EQ(m.project.sequence().fragments().size(), 2u);
    EXPECT_EQ(m.project.audioTracks().find(m.music), nullptr);
    // Il pezzo B resta, senza più gruppo (dal gruppo è rimasto solo lui).
    EXPECT_EQ(m.project.groupMoverCount({Kind::Piece, m.b}), 1);
}

TEST(GroupSplitAndRemove, GroupingAGroupWithAnotherObjectMakesOneTotalGroup)
{
    Montage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}}), 0);
    // Il gruppo (A, B) più la clip: si sceglie un oggetto del gruppo e la clip, e si porta tutto il gruppo.
    std::vector<ObjectRef> chosen{{Kind::Piece, m.a}, {Kind::AudioClip, m.music}};
    for (const ObjectRef& o : std::vector<ObjectRef>(chosen)) {
        for (const ObjectRef& mate : m.project.groupMembers(o)) {
            if (std::find(chosen.begin(), chosen.end(), mate) == chosen.end())
                chosen.push_back(mate);
        }
    }
    ASSERT_NE(m.project.groupObjects(chosen), 0);
    EXPECT_EQ(m.project.groupMoverCount({Kind::Piece, m.b}), 3);
    EXPECT_EQ(m.project.groupMoverCount({Kind::AudioClip, m.music}), 3);
}

TEST(GroupMove, MoveObjectSingleAndGroup)
{
    Montage m;
    // Da solo: come Sequence::moveTo.
    Project::MoveResult single = m.project.moveObject({Kind::Piece, m.a}, 0, 0.5, false);
    EXPECT_TRUE(single.moved);
    EXPECT_FALSE(single.group);
    EXPECT_NEAR(m.start(m.a), 0.5, 1e-6);
    EXPECT_NEAR(m.project.freeSpotFor({Kind::Piece, m.a}, 0, 0.25, false), m.project.sequence().nearestFreeStart(m.a, 0.25, false), 1e-9);
    EXPECT_NEAR(m.project.freeSpotFor({Kind::AudioClip, m.music}, m.track, 3.5, false),
                m.project.audioTracks().nearestFreePosition(m.music, m.track, 3.5), 1e-9);

    // In gruppo: si muove tutto il gruppo, entro lo spazio libero.
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.b}, {Kind::AudioClip, m.music}}), 0);
    EXPECT_NEAR(m.project.freeSpotFor({Kind::Piece, m.b}, 0, 9.0, false), 4.0, 1e-6);  // B finisce a 5, C parte a 6
    const Project::MoveResult group = m.project.moveObject({Kind::AudioClip, m.music}, m.track, 3.5, false);
    EXPECT_TRUE(group.moved);
    EXPECT_TRUE(group.group);
    EXPECT_NEAR(m.start(m.b), 3.5, 1e-6);
    EXPECT_NEAR(m.clipAt(), 3.5, 1e-6);
    // Uno spostamento minuscolo non è una modifica.
    EXPECT_FALSE(m.project.moveObject({Kind::Piece, m.b}, 0, 3.5001, false).moved);
    // Oggetto che non esiste.
    EXPECT_FALSE(m.project.moveObject({Kind::Piece, 9999}, 0, 1.0, false).moved);
    EXPECT_LT(m.project.freeSpotFor({Kind::AudioClip, 9999}, m.track, 1.0, false), 0.0);
}

// Un montaggio grande con un gruppo: i calcoli fatti a ogni movimento del mouse devono restare rapidi.
TEST(GroupMove, ManyObjectsStayFast)
{
    Project project;
    const int file = project.add("video.mp4");
    project.setAnalysis(file, {0.0}, 5000.0);
    constexpr int kPieces = 500;
    std::vector<int> pieces;
    for (int i = 0; i < kPieces; ++i)
        pieces.push_back(project.sequence().insert(-1, file, i * 2.0, i * 2.0 + 1.0));
    ASSERT_EQ(static_cast<int>(pieces.size()), kPieces);
    ASSERT_NE(project.groupObjects({{Kind::Piece, pieces[10]}, {Kind::Piece, pieces[11]}}), 0);

    const auto begin = std::chrono::steady_clock::now();
    double sum = 0.0;
    for (int i = 0; i < 20; ++i)
        sum += project.freeSpotFor({Kind::Piece, pieces[10]}, 0, 20.0 + i * 0.01, true);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    EXPECT_GT(sum, 0.0);
    EXPECT_LT(seconds / 20.0, 0.05) << "più di 50 ms per un movimento";
}
