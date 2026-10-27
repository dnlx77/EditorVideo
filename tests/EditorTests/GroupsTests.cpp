#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace editor::core;

namespace {

using Kind = ObjectRef::Kind;

struct Montage
{
    Project project;
    int a = 0;  // tre pezzi video di 2 s: 0 .. 2, 2 .. 4, 4 .. 6
    int b = 0;
    int c = 0;
    int music = 0;  // una clip audio su A2

    Montage()
    {
        const int file = project.add("video.mp4");
        project.setAnalysis(file, {0.0}, 30.0);
        a = project.sequence().insert(-1, file, 0.0, 2.0);
        b = project.sequence().insert(-1, file, 4.0, 6.0);
        c = project.sequence().insert(-1, file, 8.0, 10.0);
        AudioClip clip;
        clip.path = "musica.mp3";
        clip.end = 3.0;
        clip.position = 1.0;
        clip.sourceDuration = 100.0;
        music = project.audioTracks().add(project.audioTracks().addTrack(), clip);
    }
};

}  // namespace

TEST(Groups, VideoAndItsAudioAreBoundFromTheStart)
{
    Montage m;
    const auto members = m.project.groupMembers({Kind::Piece, m.a});
    ASSERT_EQ(members.size(), 2u);
    EXPECT_EQ(members[0], (ObjectRef{Kind::Piece, m.a}));
    EXPECT_EQ(members[1], (ObjectRef{Kind::PieceAudio, m.a}));
    EXPECT_EQ(m.project.groupMembers({Kind::PieceAudio, m.a}), members);
    // Una clip audio libera non è legata a niente.
    EXPECT_EQ(m.project.groupMembers({Kind::AudioClip, m.music}).size(), 1u);
    // Un oggetto che non c'è non ha gruppo.
    EXPECT_TRUE(m.project.groupMembers({Kind::Piece, 999}).empty());
}

TEST(Groups, CreatingAGroupBindsEverythingOfItsMembers)
{
    Montage m;
    const int id = m.project.groupObjects({{Kind::Piece, m.a}, {Kind::AudioClip, m.music}});
    ASSERT_NE(id, 0);
    // Il gruppo porta con sé anche l'audio del pezzo A (legame naturale).
    const auto members = m.project.groupMembers({Kind::AudioClip, m.music});
    ASSERT_EQ(members.size(), 3u);
    EXPECT_NE(std::find(members.begin(), members.end(), ObjectRef{Kind::PieceAudio, m.a}), members.end());
    // B resta fuori.
    EXPECT_EQ(m.project.groupMembers({Kind::Piece, m.b}).size(), 2u);
    // Pezzo e audio sono già legati: niente gruppo nuovo. Un solo oggetto: neanche.
    EXPECT_EQ(m.project.groupObjects({{Kind::Piece, m.b}, {Kind::PieceAudio, m.b}}), 0);
    EXPECT_EQ(m.project.groupObjects({{Kind::Piece, m.b}}), 0);
}

TEST(Groups, AnObjectIsInOneGroupOnly)
{
    Montage m;
    const int first = m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}});
    ASSERT_NE(first, 0);
    // B passa nel gruppo nuovo con C: il primo resta con un solo oggetto e si scioglie.
    const int second = m.project.groupObjects({{Kind::Piece, m.b}, {Kind::Piece, m.c}});
    ASSERT_NE(second, 0);
    EXPECT_EQ(m.project.groups().find(first), nullptr);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.b}), second);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.a}), 0);
}

TEST(Groups, UngroupDissolvesTheGroupThenUnlinksTheAudio)
{
    Montage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}}), 0);
    // Prima U scioglie il gruppo dell'utente.
    auto result = m.project.ungroupObjects({{Kind::Piece, m.a}});
    EXPECT_EQ(result.dissolved, 1);
    EXPECT_EQ(result.audioUnlinked, 0);
    EXPECT_TRUE(m.project.groups().empty());
    // Poi, su un pezzo che ha solo il suo audio, scollega l'audio: diventa una clip qualsiasi.
    result = m.project.ungroupObjects({{Kind::Piece, m.a}});
    EXPECT_EQ(result.dissolved, 0);
    EXPECT_EQ(result.audioUnlinked, 1);
    EXPECT_TRUE(m.project.sequence().find(m.a)->detached);
    EXPECT_EQ(m.project.groupMembers({Kind::Piece, m.a}).size(), 1u);
    EXPECT_FALSE(m.project.objectExists({Kind::PieceAudio, m.a}));
    // Niente più da sciogliere.
    EXPECT_EQ(m.project.ungroupObjects({{Kind::Piece, m.a}}).audioUnlinked, 0);
}

TEST(Groups, GonePiecesLeaveTheGroups)
{
    Montage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}, {Kind::AudioClip, m.music}}), 0);
    ASSERT_TRUE(m.project.sequence().remove(m.b));
    EXPECT_EQ(m.project.groupMembers({Kind::Piece, m.a}).size(), 3u);  // A, il suo audio, la musica
    ASSERT_TRUE(m.project.audioTracks().remove(m.music));
    EXPECT_EQ(m.project.groupMembers({Kind::Piece, m.a}).size(), 2u);  // di gruppo dell'utente non resta niente
    EXPECT_TRUE(m.project.validGroups().empty());
}

TEST(Groups, UndoAndRedoKeepTheGroups)
{
    Montage m;
    const Project before = m.project;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}}), 0);
    const Project grouped = m.project;
    m.project.restoreEdits(before);
    EXPECT_TRUE(m.project.groups().empty());
    m.project.restoreEdits(grouped);
    EXPECT_EQ(m.project.groups().all().size(), 1u);
    EXPECT_EQ(m.project.groupMembers({Kind::Piece, m.b}).size(), 4u);
}

TEST(Groups, AreSavedAndReopened)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "EditorVideoGroupsTests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "gruppi.evproj").string();
    Montage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.b}, {Kind::AudioClip, m.music}}), 0);
    ASSERT_EQ(m.project.ungroupObjects({{Kind::Piece, m.c}}).audioUnlinked, 1);  // C scollegato: audio libero su A1
    saveProjectFile(path, m.project, ProjectView{});
    const LoadedProject loaded = loadProjectFile(path);
    const Project& again = loaded.project;
    ASSERT_EQ(again.sequence().fragments().size(), 3u);
    const int b = again.sequence().fragments()[1].id;
    const int c = again.sequence().fragments()[2].id;
    ASSERT_EQ(again.validGroups().size(), 1u);
    const auto members = again.groupMembers({Kind::Piece, b});
    EXPECT_EQ(members.size(), 3u);  // B, il suo audio, la musica
    EXPECT_TRUE(again.sequence().find(c)->detached);
    EXPECT_EQ(again.groupMembers({Kind::Piece, c}).size(), 1u);
    std::filesystem::remove_all(dir);
}

TEST(Groups, GroupAfterUngroupBindsVideoAndItsAudioAgain)
{
    Montage m;
    const int clip = m.project.unlinkAudio(m.a);
    ASSERT_NE(clip, 0);
    ASSERT_TRUE(m.project.sequence().find(m.a)->detached);

    int relinked = 0;
    EXPECT_EQ(m.project.groupObjects({{Kind::Piece, m.a}, {Kind::AudioClip, clip}}, &relinked), 0);
    EXPECT_EQ(relinked, 1);
    EXPECT_FALSE(m.project.sequence().find(m.a)->detached);
    EXPECT_EQ(m.project.audioTracks().find(clip), nullptr);
    EXPECT_TRUE(m.project.objectExists({Kind::PieceAudio, m.a}));
    EXPECT_TRUE(m.project.sequence().mutedRanges().empty());
}

TEST(Groups, SplitKeepsTheUnlinkedAudioUnlinkedInBothHalves)
{
    Montage m;
    ASSERT_EQ(m.project.ungroupObjects({{Kind::Piece, m.a}}).audioUnlinked, 1);
    const auto created = m.project.splitGroup({Kind::Piece, m.a}, 1.0);
    ASSERT_FALSE(created.empty());
    const int second = created.front().id;
    EXPECT_TRUE(m.project.sequence().find(m.a)->detached);
    EXPECT_TRUE(m.project.sequence().find(second)->detached);
}
