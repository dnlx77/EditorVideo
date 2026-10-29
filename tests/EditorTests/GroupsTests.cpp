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

TEST(Groups, EditingOneObjectAtATime)
{
    Montage m;
    const int g1 = m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}});
    const int g2 = m.project.groupObjects({{Kind::Piece, m.c}, {Kind::AudioClip, m.music}});
    ASSERT_NE(g1, 0);
    ASSERT_NE(g2, 0);
    // Un oggetto passa da un gruppo all'altro; il montaggio non cambia.
    const double startOfB = m.project.sequence().startOf(m.project.sequence().indexOf(m.b));
    EXPECT_TRUE(m.project.addToGroup(g2, {Kind::Piece, m.b}));
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.b}), g2);
    EXPECT_EQ(m.project.groups().find(g2)->members.size(), 3u);
    EXPECT_EQ(m.project.sequence().startOf(m.project.sequence().indexOf(m.b)), startOfB);
    // Il gruppo 1 è rimasto con il solo A: si scioglie.
    EXPECT_EQ(m.project.groups().find(g1), nullptr);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.a}), 0);
    // Già nel gruppo, gruppo o oggetto che non ci sono: niente da fare.
    EXPECT_FALSE(m.project.addToGroup(g2, {Kind::Piece, m.b}));
    EXPECT_FALSE(m.project.addToGroup(999, {Kind::Piece, m.a}));
    EXPECT_FALSE(m.project.addToGroup(g2, {Kind::Piece, 999}));
    // Aggiungere un oggetto libero.
    EXPECT_TRUE(m.project.addToGroup(g2, {Kind::Piece, m.a}));
    EXPECT_EQ(m.project.groups().find(g2)->members.size(), 4u);
    // Toglierne uno lascia il gruppo; toglierne fino a uno solo lo scioglie e libera anche l'ultimo.
    EXPECT_TRUE(m.project.removeFromGroup({Kind::Piece, m.a}));
    EXPECT_FALSE(m.project.removeFromGroup({Kind::Piece, m.a}));
    EXPECT_TRUE(m.project.removeFromGroup({Kind::Piece, m.b}));
    EXPECT_TRUE(m.project.removeFromGroup({Kind::AudioClip, m.music}));
    EXPECT_TRUE(m.project.groups().all().empty());
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.c}), 0);
    // Sempre tutto al suo posto.
    EXPECT_EQ(m.project.sequence().fragments().size(), 3u);
}

TEST(Groups, NamesAreAutomaticUntilGiven)
{
    Montage m;
    const int id = m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}});
    ASSERT_NE(id, 0);
    EXPECT_EQ(m.project.groups().find(id)->label(), "Gruppo " + std::to_string(id));
    EXPECT_TRUE(m.project.renameGroup(id, "Introduzione"));
    EXPECT_FALSE(m.project.renameGroup(id, "Introduzione"));
    EXPECT_FALSE(m.project.renameGroup(999, "Altro"));
    EXPECT_EQ(m.project.groups().find(id)->label(), "Introduzione");
    // Annulla e Ripeti riportano anche il nome.
    const Project named = m.project;
    ASSERT_TRUE(m.project.renameGroup(id, ""));
    EXPECT_EQ(m.project.groups().find(id)->label(), "Gruppo " + std::to_string(id));
    m.project.restoreEdits(named);
    EXPECT_EQ(m.project.groups().find(id)->label(), "Introduzione");
}

TEST(Groups, NamesAreSavedAndOldFilesHaveNone)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "EditorVideoGroupNamesTests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "nomi.evproj").string();
    Montage m;
    const int g1 = m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}});
    const int g2 = m.project.groupObjects({{Kind::Piece, m.c}, {Kind::AudioClip, m.music}});
    ASSERT_NE(g1, 0);
    ASSERT_NE(g2, 0);
    // Senza nomi il file non ha la chiave dei nomi (si apre anche con i programmi di prima).
    saveProjectFile(path, m.project, ProjectView{});
    {
        std::ifstream in(path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("nomiGruppi"), std::string::npos);
    }
    ASSERT_TRUE(m.project.renameGroup(g2, "Finale"));
    saveProjectFile(path, m.project, ProjectView{});
    const LoadedProject loaded = loadProjectFile(path);
    const auto groups = loaded.project.validGroupList();
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_TRUE(groups[0].name.empty());
    EXPECT_EQ(groups[1].name, "Finale");
    EXPECT_EQ(groups[1].members.size(), 2u);
    std::filesystem::remove_all(dir);
}

TEST(Groups, DroppingObjectsOnTheGroupList)
{
    Montage m;
    const int g1 = m.project.groupObjects({{Kind::Piece, m.a}, {Kind::Piece, m.b}});
    const int g2 = m.project.groupObjects({{Kind::Piece, m.c}, {Kind::AudioClip, m.music}});
    ASSERT_NE(g1, 0);
    ASSERT_NE(g2, 0);
    const double startOfC = m.project.sequence().startOf(m.project.sequence().indexOf(m.c));
    // Su un gruppo: l'oggetto passa lì (e il gruppo che resta con uno solo si scioglie); su quello in cui è già, niente.
    EXPECT_FALSE(m.project.dropOnGroup({{Kind::Piece, m.c}}, g2));
    EXPECT_TRUE(m.project.dropOnGroup({{Kind::Piece, m.a}}, g2));
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.a}), g2);
    EXPECT_EQ(m.project.groups().find(g1), nullptr);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.b}), 0);
    // Più oggetti insieme su un gruppo.
    EXPECT_TRUE(m.project.dropOnGroup({{Kind::Piece, m.b}, {Kind::Piece, m.a}}, g2));
    EXPECT_EQ(m.project.groups().find(g2)->members.size(), 4u);
    // Uno solo nello spazio vuoto: esce dal gruppo; uno già libero: niente.
    EXPECT_TRUE(m.project.dropOnGroup({{Kind::Piece, m.b}}, 0));
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.b}), 0);
    EXPECT_FALSE(m.project.dropOnGroup({{Kind::Piece, m.b}}, 0));
    // Due o più nello spazio vuoto: un gruppo nuovo (quello vecchio che resta con uno solo si scioglie).
    EXPECT_TRUE(m.project.dropOnGroup({{Kind::Piece, m.a}, {Kind::AudioClip, m.music}}, 0));
    const int fresh = m.project.groups().groupOf({Kind::Piece, m.a});
    EXPECT_NE(fresh, 0);
    EXPECT_NE(fresh, g2);
    EXPECT_EQ(m.project.groups().groupOf({Kind::AudioClip, m.music}), fresh);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.c}), 0);
    // Gruppo o oggetti che non ci sono: niente.
    EXPECT_FALSE(m.project.dropOnGroup({{Kind::Piece, m.b}}, 999));
    EXPECT_FALSE(m.project.dropOnGroup({{Kind::Piece, 999}}, fresh));
    EXPECT_FALSE(m.project.dropOnGroup({}, fresh));
    // Il montaggio non si è mosso.
    EXPECT_EQ(m.project.sequence().startOf(m.project.sequence().indexOf(m.c)), startOfC);
    EXPECT_EQ(m.project.sequence().fragments().size(), 3u);
    // Annulla e Ripeti.
    const Project after = m.project;
    Project before = m.project;
    ASSERT_TRUE(m.project.dropOnGroup({{Kind::Piece, m.a}}, 0));
    m.project.restoreEdits(before);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.a}), fresh);
    m.project.restoreEdits(after);
    EXPECT_EQ(m.project.groups().groupOf({Kind::Piece, m.a}), fresh);
}

TEST(Groups, DroppingOnAVideoWithItsAudioMakesANewGroupOfAll)
{
    Montage m;
    // Come dall'elenco: la musica trascinata sul video B con il suo audio (già legati, non in un gruppo dell'utente).
    const double startOfB = m.project.sequence().startOf(m.project.sequence().indexOf(m.b));
    EXPECT_TRUE(m.project.dropOnGroup({{Kind::AudioClip, m.music}, {Kind::Piece, m.b}, {Kind::PieceAudio, m.b}}, 0));
    const int id = m.project.groups().groupOf({Kind::Piece, m.b});
    ASSERT_NE(id, 0);
    EXPECT_EQ(m.project.groups().find(id)->members.size(), 3u);
    EXPECT_EQ(m.project.groups().groupOf({Kind::AudioClip, m.music}), id);
    EXPECT_EQ(m.project.groups().groupOf({Kind::PieceAudio, m.b}), id);
    EXPECT_EQ(m.project.sequence().startOf(m.project.sequence().indexOf(m.b)), startOfB);
    EXPECT_TRUE(m.project.sequence().find(m.b) != nullptr && !m.project.sequence().find(m.b)->detached);
}
