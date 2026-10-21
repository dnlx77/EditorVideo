#include "EditorCore/EditHistory.h"
#include "EditorCore/FileDrop.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/ProjectFile.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using namespace editor::core;

namespace {

// Cartella con file finti: per la logica conta solo se esistono; la "lettura" la fa un ispettore finto.
class FileDropTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() / "EditorVideoFileDropTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "cartella" / "sotto");
        for (const char* name : {"a.mp4", "b.mkv", "note.txt", "rotto.mp4", "lavoro.evproj", "cartella/c.mp3",
                                 "cartella/B.mov", "cartella/leggimi.txt", "cartella/sotto/interno.mp4"})
            std::ofstream(dir_ / name) << "x";
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::string path(const std::string& name) const { return utf8FromPath((dir_ / name).make_preferred()); }

    // Durata finta per nome; "rotto" e i .txt non sono leggibili.
    static std::optional<double> inspect(const std::string& file)
    {
        static const std::map<std::string, double> durations = {
            {"a.mp4", 10.0}, {"b.mkv", 20.0}, {"c.mp3", 30.0}, {"B.mov", 5.0}, {"interno.mp4", 1.0}};
        const auto it = durations.find(utf8FromPath(pathFromUtf8(file).filename()));
        return it != durations.end() ? std::optional<double>(it->second) : std::nullopt;
    }

    static std::vector<std::string> names(const DropPlan& plan)
    {
        std::vector<std::string> out;
        for (const DroppedMedia& m : plan.media)
            out.push_back(utf8FromPath(pathFromUtf8(m.path).filename()));
        return out;
    }

    fs::path dir_;
};

TEST_F(FileDropTest, FilesFoldersAndUnreadableOnes)
{
    const Project empty;
    const DropPlan plan = planDrop({path("a.mp4"), path("cartella"), path("note.txt"), path("rotto.mp4")}, empty, inspect);
    EXPECT_FALSE(plan.project.has_value());
    // La cartella dà i suoi video e audio in ordine di nome, senza le sottocartelle e senza gli altri file.
    EXPECT_EQ(names(plan), (std::vector<std::string>{"a.mp4", "B.mov", "c.mp3"}));
    EXPECT_EQ(plan.unreadable, (std::vector<std::string>{"note.txt", "rotto.mp4"}));
    ASSERT_EQ(plan.media.size(), 3u);
    EXPECT_EQ(plan.media[1].duration, 5.0);
    EXPECT_EQ(plan.media[0].existingClipId, 0);
}

TEST_F(FileDropTest, AProjectIsOpenedInstead)
{
    const DropPlan plan = planDrop({path("a.mp4"), path("lavoro.evproj"), path("b.mkv")}, Project{}, inspect);
    ASSERT_TRUE(plan.project.has_value());
    EXPECT_EQ(utf8FromPath(pathFromUtf8(*plan.project).filename()), "lavoro.evproj");
    EXPECT_TRUE(plan.media.empty());
}

TEST_F(FileDropTest, NoDuplicates)
{
    Project project;
    // Già nel progetto, scritto con maiuscole e "/" diverse.
    std::string upper = path("a.mp4");
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::replace(upper.begin(), upper.end(), '\\', '/');
    const int existing = project.add(upper);
    project.setAnalysis(existing, {0.0}, 9.5);

    const DropPlan plan = planDrop({path("a.mp4"), path("b.mkv"), path("b.mkv"), path("cartella/B.mov"), path("cartella")},
                                   project, inspect);
    EXPECT_EQ(names(plan), (std::vector<std::string>{"A.MP4", "b.mkv", "B.mov", "c.mp3"}));
    EXPECT_EQ(plan.media[0].existingClipId, existing);
    EXPECT_EQ(plan.media[0].duration, 9.5);  // quella dell'analisi

    const std::vector<int> ids = applyDrop(project, plan.media, -1);
    EXPECT_EQ(ids.front(), existing);
    EXPECT_EQ(project.clips().size(), 4u);  // a (già c'era) + b, B, c
    EXPECT_TRUE(project.sequence().empty());  // senza posizione nella sequenza: solo l'elenco dei file
}

TEST_F(FileDropTest, InsertedWholeAtTheDropPosition)
{
    Project project;
    const int x = project.add(path("x.mp4"));
    project.setAnalysis(x, {0.0, 2.0}, 8.0);
    project.sequence().insert(-1, x, 0.0, 4.0, project.snapper());
    project.sequence().insert(-1, x, 4.0, 8.0, project.snapper());

    // Due file nuovi e x (già nel progetto) rilasciati fra i due pezzi: nell'ordine del trascinamento.
    const DropPlan plan = planDrop({path("b.mkv"), path("a.mp4")}, project, inspect);
    std::vector<DroppedMedia> media = plan.media;
    media.push_back({project.find(x)->path, 8.0, x});
    std::vector<int> fragments;
    const std::vector<int> ids = applyDrop(project, media, 1, &fragments);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(fragments.size(), 3u);

    const auto& f = project.sequence().fragments();
    ASSERT_EQ(f.size(), 5u);
    EXPECT_EQ(f[0].clipId, x);
    EXPECT_EQ(f[1].clipId, ids[0]);  // b
    EXPECT_EQ(f[2].clipId, ids[1]);  // a
    EXPECT_EQ(f[3].clipId, x);       // x intero
    EXPECT_EQ(f[4].clipId, x);
    EXPECT_DOUBLE_EQ(f[1].length(), 20.0);  // interi
    EXPECT_DOUBLE_EQ(f[2].length(), 10.0);
    EXPECT_DOUBLE_EQ(f[3].start, 0.0);
    EXPECT_DOUBLE_EQ(f[3].end, 8.0);
    EXPECT_EQ(project.clips().size(), 3u);

    // In fondo: una posizione oltre la fine va in coda.
    applyDrop(project, {{path("c.mp3"), 30.0, 0}}, 99);
    EXPECT_EQ(project.sequence().fragments().back().clipId, project.clips().back().id);
}

TEST_F(FileDropTest, TheWholeDropIsOneUndo)
{
    Project project;
    const int x = project.add(path("x.mp4"));
    project.setAnalysis(x, {0.0}, 8.0);
    project.sequence().insert(-1, x, 0.0, 8.0, project.snapper());
    EditHistory history;
    const std::string before = projectToJson("C:\\p.evproj", project, {});

    const Project snapshot = project;
    applyDrop(project, planDrop({path("a.mp4"), path("cartella")}, project, inspect).media, 0);
    history.record("Aggiungi file", snapshot);
    const std::string after = projectToJson("C:\\p.evproj", project, {});
    EXPECT_NE(after, before);
    EXPECT_EQ(history.undoLabel(), "Aggiungi file");

    ASSERT_TRUE(history.undo(project));
    EXPECT_EQ(projectToJson("C:\\p.evproj", project, {}), before);  // spariti file e pezzi in un colpo solo
    ASSERT_TRUE(history.redo(project));
    EXPECT_EQ(projectToJson("C:\\p.evproj", project, {}), after);
}

TEST(FileDrop, InspectRealFiles)
{
    if (!editor::tests::sampleEncodersAvailable())
        GTEST_SKIP() << "FFmpeg senza gli encoder necessari";
    const fs::path dir = fs::temp_directory_path() / "EditorVideoInspectTests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    editor::tests::writeSampleFile(dir / "video.mkv", {.video = true, .audio = true, .frames = 50});
    editor::tests::writeSampleFile(dir / "audio.wav", {.video = false, .audio = true, .frames = 25});
    std::ofstream(dir / "finto.mp4") << "non sono un video";

    const auto video = inspectMedia(utf8FromPath(dir / "video.mkv"));
    ASSERT_TRUE(video.has_value());
    EXPECT_NEAR(*video, 2.0, 0.05);
    EXPECT_NEAR(inspectMedia(utf8FromPath(dir / "audio.wav")).value_or(-1), 1.0, 0.05);
    EXPECT_FALSE(inspectMedia(utf8FromPath(dir / "finto.mp4")).has_value());
    EXPECT_FALSE(inspectMedia(utf8FromPath(dir / "non_esiste.mp4")).has_value());
    EXPECT_FALSE(inspectMedia(utf8FromPath(dir)).has_value());  // una cartella non è un file
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace
