#include "EditorCore/EditHistory.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Thumbnails.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::lumaAt;
using editor::tests::sampleLuma;
using editor::tests::SampleSpec;
using editor::tests::SampleDirTest;
using editor::tests::writeSampleFile;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Annulla / Ripeti

// Tutto ciò che il progetto salverebbe: due progetti con lo stesso testo sono identici.
std::string state(const Project& project)
{
    return projectToJson("C:\\prova\\x.evproj", project, {});
}

Project start()
{
    Project project;
    const int a = project.add("C:\\video\\a.mp4");
    const int b = project.add("C:\\video\\b.mkv");
    project.setAnalysis(a, {0.0, 1.0, 2.0, 3.0, 4.0}, 5.0);
    project.setAnalysis(b, {0.0, 2.0, 4.0}, 6.0);
    return project;
}

TEST(EditHistory, EveryActionUndoesAndRedoesExactly)
{
    Project project = start();
    EditHistory history;
    std::vector<std::string> states{state(project)};
    std::vector<std::string> labels;
    const auto apply = [&](const std::string& label, const std::function<void()>& change) {
        const Project before = project;
        change();
        history.record(label, before);
        states.push_back(state(project));
        labels.push_back(label);
    };
    Sequence& s = project.sequence();
    apply("Aggiungi pezzo", [&] { s.insert(-1, 1, 0.5, 3.5); });
    apply("Aggiungi pezzo", [&] { s.insert(-1, 2, 2.0, 6.0); });
    apply("Sposta pezzo", [&] { s.move(s.fragments()[1].id, 0); });
    apply("Sposta pezzo", [&] { s.moveTo(s.fragments()[1].id, s.startOf(1) + 2.0); });  // lascia uno spazio vuoto
    apply("Chiudi spazio vuoto", [&] { s.closeGap(1); });
    apply("Sposta pezzo", [&] { s.moveTo(s.fragments()[1].id, s.startOf(1) + 0.1); });  // (il pezzo si divide ancora a 5,2)
    apply("Dividi pezzo", [&] { s.split(5.2); });
    apply("Togli IN-OUT", [&] { s.removeSourceRange(1, 1.2, 2.5); });
    apply("Togli audio", [&] { s.muteRange(0.5, 1.5); });
    apply("Rimetti audio", [&] { s.unmuteRange(0.8, 1.0); });
    apply("Sostituisci audio", [&] { project.setAudioReplacement(AudioReplacement{"C:\\musica.mp3", 0.0, 90.0}); });
    apply("Sposta audio", [&] { project.setAudioOffset(-2.5); });
    apply("Elimina pezzo", [&] { s.remove(s.fragments().front().id); });
    apply("Aggiungi file", [&] { project.add("C:\\video\\c.mov"); });
    apply("Togli file", [&] { project.remove(2); });

    // Ogni stato è diverso dal precedente (altrimenti la prova non proverebbe niente).
    for (std::size_t i = 1; i < states.size(); ++i)
        EXPECT_NE(states[i], states[i - 1]) << labels[i - 1];

    // Annullando tutto si ripercorrono gli stati all'indietro, fino a quello iniziale...
    for (std::size_t i = labels.size(); i-- > 0;) {
        EXPECT_EQ(history.undoLabel(), labels[i]);
        ASSERT_TRUE(history.undo(project));
        EXPECT_EQ(state(project), states[i]) << "annullando " << labels[i];
    }
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.undo(project));

    // ...e ripetendo tutto si torna alla fine.
    for (std::size_t i = 0; i < labels.size(); ++i) {
        EXPECT_EQ(history.redoLabel(), labels[i]);
        ASSERT_TRUE(history.redo(project));
        EXPECT_EQ(state(project), states[i + 1]) << "ripetendo " << labels[i];
    }
    EXPECT_FALSE(history.canRedo());

    // Una modifica nuova dopo un annullamento toglie la possibilità di ripetere.
    history.undo(project);
    EXPECT_TRUE(history.canRedo());
    const Project before = project;
    project.sequence().clear();
    history.record("Svuota il montaggio", before);
    EXPECT_FALSE(history.canRedo());
    EXPECT_EQ(history.undoLabel(), "Svuota il montaggio");
}

TEST(EditHistory, CleanStateFollowsSaving)
{
    Project project = start();
    EditHistory history;
    EXPECT_TRUE(history.isClean());
    const auto edit = [&](double at) {
        const Project before = project;
        project.sequence().insert(-1, 1, at, at + 1.0);
        history.record("Aggiungi pezzo", before);
    };
    edit(0.0);
    EXPECT_FALSE(history.isClean());
    edit(1.0);
    history.setClean();  // salvato qui
    EXPECT_TRUE(history.isClean());
    history.undo(project);
    EXPECT_FALSE(history.isClean());
    history.redo(project);
    EXPECT_TRUE(history.isClean());  // di nuovo come salvato

    // Annullato e poi modificato diversamente: lo stato salvato non si ritrova più.
    history.undo(project);
    edit(3.0);
    EXPECT_FALSE(history.isClean());
    history.undo(project);
    EXPECT_FALSE(history.isClean());
    history.clear();
    EXPECT_TRUE(history.isClean());
}

TEST(EditHistory, LimitDropsTheOldestSteps)
{
    Project project = start();
    EditHistory history(3);
    for (int i = 0; i < 5; ++i) {
        const Project before = project;
        project.sequence().insert(-1, 1, 0.0, 1.0);
        history.record("Aggiungi pezzo", before);
    }
    int undone = 0;
    while (history.undo(project))
        ++undone;
    EXPECT_EQ(undone, 3);
    EXPECT_EQ(project.sequence().fragments().size(), 2u);
}

TEST(EditHistory, UndoKeepsTheAnalysisDoneMeanwhile)
{
    // Un file aggiunto e messo nel montaggio prima che l'analisi finisca: annullando l'ultimo passo il file non deve
    // tornare "da analizzare".
    Project project;
    EditHistory history;
    Project before = project;
    const int id = project.add("C:\\video\\a.mp4");
    history.record("Aggiungi file", before);
    before = project;
    project.sequence().insert(-1, id, 0.0, 2.0);
    history.record("Aggiungi pezzo", before);
    project.setAnalysis(id, {0.0, 1.0}, 4.0);  // l'analisi finisce adesso

    history.undo(project);
    EXPECT_TRUE(project.sequence().empty());
    ASSERT_NE(project.find(id), nullptr);
    EXPECT_TRUE(project.find(id)->analyzed);
    EXPECT_EQ(project.find(id)->duration, 4.0);
    history.undo(project);
    EXPECT_TRUE(project.empty());
    history.redo(project);
    EXPECT_TRUE(project.find(id)->analyzed);  // rimesso con l'analisi che aveva
    // Un file aggiunto dopo non riprende l'id di quello tolto.
    history.undo(project);
    EXPECT_NE(project.add("C:\\video\\b.mp4"), id);
}

// ---------------------------------------------------------------------------------------------------------------
// Miniature ed esportazione annullata

class ComfortMediaTest : public SampleDirTest<ComfortMediaTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoComfortTests";

    static void prepareSamples()
    {
        writeSampleFile(dir_ / "video.mkv", {.video = true, .audio = true, .frames = 50, .gopSize = 10});
        writeSampleFile(dir_ / "audio.wav", {.video = false, .audio = true, .frames = 50});
    }

protected:
};

TEST_F(ComfortMediaTest, ThumbnailIsTheFrameAtThatTime)
{
    ThumbnailExtractor thumbnails(32, 32, false);
    for (int index : {0, 10, 20, 30, 40}) {
        const auto frame = thumbnails.extract(path("video.mkv"), index / 25.0);
        ASSERT_NE(frame, nullptr) << index;
        // 64x48 nel riquadro 32x32: 32x24, proporzioni mantenute.
        EXPECT_EQ(frame->width, 32);
        EXPECT_EQ(frame->height, 24);
        EXPECT_NEAR(frame->time, index / 25.0, 1e-6);
        // Il centro della miniatura è il centro del fotogramma originale.
        EXPECT_NEAR(lumaAt(*frame, 16, 12), sampleLuma(32, 24, index), 4.0) << "fotogramma " << index;
    }
    EXPECT_EQ(thumbnails.decoderDescription(), "software");
    EXPECT_EQ(thumbnails.extract(path("audio.wav"), 0.0), nullptr);  // niente video
    EXPECT_EQ(thumbnails.extract(path("non_esiste.mkv"), 0.0), nullptr);
    EXPECT_NE(thumbnails.extract(path("video.mkv"), 0.8), nullptr);  // si torna al file di prima

    // Con la GPU permessa (se c'è) il risultato è lo stesso fotogramma.
    ThumbnailExtractor gpu(160, 90, true);
    const auto frame = gpu.extract(path("video.mkv"), 0.8);
    ASSERT_NE(frame, nullptr);
    EXPECT_LE(frame->width, 160);  // mai ingrandita oltre l'originale
    EXPECT_NEAR(lumaAt(*frame, frame->width / 2, frame->height / 2), sampleLuma(32, 24, 20), 4.0);
}

TEST_F(ComfortMediaTest, CancelledExportLeavesNoFile)
{
    // Montaggio con l'audio sostituito, annullato a metà: nessun file a metà.
    const std::string out = path("annullato.mkv");
    AudioEdit audio;
    audio.replacement = AudioReplacement{path("audio.wav"), 0.5, 2.0};
    audio.muted = {{0.2, 0.4}};
    double reached = 0.0;
    EXPECT_THROW(exportClips({{path("video.mkv"), {{0.0, 1.0}}}, {path("video.mkv"), {{1.0, 2.0}}}}, out, audio,
                             [&](double fraction) {
                                 reached = fraction;
                                 return fraction < 0.5;
                             }),
                 ExportCancelled);
    EXPECT_GE(reached, 0.5);
    EXPECT_LT(reached, 1.0);
    EXPECT_FALSE(fs::exists(dir_ / "annullato.mkv"));

    // Senza annullare lo stesso montaggio si esporta, e l'avanzamento arriva al 100%.
    double last = 0.0;
    exportClips({{path("video.mkv"), {{0.0, 1.0}}}, {path("video.mkv"), {{1.0, 2.0}}}}, out, audio, [&](double fraction) {
        EXPECT_GE(fraction, last - 0.05);  // non torna indietro (salvo piccoli scarti fra video e audio)
        last = fraction;
        return true;
    });
    EXPECT_EQ(last, 1.0);
    EXPECT_TRUE(fs::exists(dir_ / "annullato.mkv"));
}

}  // namespace
