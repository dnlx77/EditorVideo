#include "EditorCore/Json.h"
#include "EditorCore/ProjectFile.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::core::utf8FromPath;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// JSON

TEST(Json, RoundTripKeepsIntegersAndText)
{
    const JsonValue::Object object{{"grande", std::int64_t{9'007'199'254'740'993}},  // oltre la precisione di un double
                                   {"testo", "Città \"vecchia\"\\C:\\video\n"},
                                   {"elenco", JsonValue::Array{1, 2.5, true, nullptr}},
                                   {"vuoto", JsonValue::Object{}}};
    const JsonValue back = parseJson(JsonValue(object).dump());
    EXPECT_EQ(back["grande"].asInteger(), 9'007'199'254'740'993);
    EXPECT_EQ(back["testo"].asString(), "Città \"vecchia\"\\C:\\video\n");
    ASSERT_EQ(back["elenco"].asArray().size(), 4u);
    EXPECT_DOUBLE_EQ(back["elenco"].asArray()[1].asNumber(), 2.5);
    EXPECT_TRUE(back["elenco"].asArray()[3].isNull());
    EXPECT_TRUE(back["vuoto"].isObject());
    EXPECT_TRUE(back["manca"].isNull());
}

TEST(Json, UnicodeEscapes)
{
    EXPECT_EQ(parseJson(R"("\u00e8 \ud83c\udfac")").asString(), "è 🎬");
}

TEST(Json, ErrorsSayWhere)
{
    try {
        parseJson("{\n  \"a\": 1,\n  \"b\" 2\n}");
        ADD_FAILURE() << "doveva essere rifiutato";
    } catch (const JsonError& e) {
        EXPECT_NE(std::string(e.what()).find("riga 3"), std::string::npos) << e.what();
    }
    for (const char* bad : {"", "{", "[1,", "\"senza fine", "{\"a\":}", "tru", "{} x", "[1 2]", "01x"})
        EXPECT_THROW(parseJson(bad), JsonError) << bad;
}

// ---------------------------------------------------------------------------------------------------------------
// Tempi

TEST(ProjectFile, TimesFromCommonTimeBasesAreExact)
{
    // Tempi come quelli che arrivano dai file: multipli di 1/90000, 1/1000, 1/48000, 1/44100, 1001/30000.
    for (long long k : {0LL, 1LL, 7LL, 12345LL, 987654321LL}) {
        for (double unit : {1.0 / 90000, 1.0 / 1000, 1.0 / 48000, 1.0 / 44100, 1001.0 / 30000, 1.0 / 25}) {
            const double t = static_cast<double>(k) * unit;
            EXPECT_EQ(ticksToSeconds(secondsToTicks(t)), ticksToSeconds(secondsToTicks(ticksToSeconds(secondsToTicks(t)))));
            EXPECT_NEAR(ticksToSeconds(secondsToTicks(t)), t, 1e-6);
        }
    }
    // E un tempo calcolato come frazione torna identico.
    const double t = 12345.0 * 1001.0 / 30000.0;
    EXPECT_EQ(ticksToSeconds(secondsToTicks(t)), t);
    EXPECT_EQ(secondsToTicks(1001.0 / 30000.0), 23'543'520);
}

// ---------------------------------------------------------------------------------------------------------------
// Salvataggio e riapertura

class ProjectFileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() / "EditorVideoProjectTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "progetto" / "video");
        // Per il progetto i file devono solo esistere: non vengono né letti né modificati.
        for (const char* name : {"video/gita.mp4", "video/città.mkv", "musica.mp3", "voce.wav", "effetto.wav"})
            write(dir_ / "progetto" / pathFromUtf8(name), "contenuto originale");
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    static void write(const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    static std::string read(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    }

    std::string file(const std::string& relative) const
    {
        return utf8FromPath((dir_ / "progetto" / pathFromUtf8(relative))
                          .lexically_normal());
    }

    std::string projectPath() const { return utf8FromPath(dir_ / "progetto" / "montaggio.evproj"); }

    // Progetto di prova: due file, quattro pezzi (uno ripetuto), audio tolto in due tratti, musica al posto
    // dell'audio originale.
    Project sample(ProjectView& view) const
    {
        Project project;
        const int a = project.add(file("video/gita.mp4"));
        const int b = project.add(file("video/città.mkv"));
        project.setAnalysis(a, {0.0, 2.002, 4.004}, 1001.0 * 300 / 30000);
        project.setAnalysis(b, {0.0, 1.0}, 12.48);
        Sequence& s = project.sequence();
        s.insert(-1, a, 2.002, 3.5035);
        s.insert(-1, b, 0.0, 0.96);
        const int third = s.insert(-1, a, 0.0, 1.001);
        s.insert(-1, b, 1.0, 5.0 + 1.0 / 48000);
        s.muteRange(1.0, 2.0);
        s.muteRange(3.1, 3.3);
        s.move(third, 0);
        project.setAudioReplacement(AudioReplacement{file("musica.mp3"), -1.25, 181.3});
        view = {b, 1.0, 2.5, 1.7, 3.25};
        return project;
    }

    static void expectSame(const Project& expected, const Project& actual)
    {
        ASSERT_EQ(actual.clips().size(), expected.clips().size());
        for (std::size_t i = 0; i < expected.clips().size(); ++i)
            EXPECT_EQ(actual.clips()[i].path, expected.clips()[i].path);
        const auto& fe = expected.sequence().fragments();
        const auto& fa = actual.sequence().fragments();
        ASSERT_EQ(fa.size(), fe.size());
        for (std::size_t i = 0; i < fe.size(); ++i) {
            // Lo stesso file (per posizione nell'elenco: gli id possono cambiare), gli stessi tempi esatti.
            EXPECT_EQ(actual.indexOf(fa[i].clipId), expected.indexOf(fe[i].clipId)) << "pezzo " << i;
            EXPECT_EQ(fa[i].start, ticksToSeconds(secondsToTicks(fe[i].start))) << "pezzo " << i;
            EXPECT_EQ(fa[i].end, ticksToSeconds(secondsToTicks(fe[i].end))) << "pezzo " << i;
            EXPECT_EQ(fa[i].gap, ticksToSeconds(secondsToTicks(fe[i].gap))) << "pezzo " << i;
            ASSERT_EQ(fa[i].muted.size(), fe[i].muted.size()) << "pezzo " << i;
            for (std::size_t m = 0; m < fe[i].muted.size(); ++m) {
                EXPECT_NEAR(fa[i].muted[m].start, fe[i].muted[m].start, 1e-9);
                EXPECT_NEAR(fa[i].muted[m].end, fe[i].muted[m].end, 1e-9);
            }
        }
        expectSameAudio(expected.audioTracks(), actual.audioTracks());
        ASSERT_EQ(actual.audioReplacement().has_value(), expected.audioReplacement().has_value());
        if (expected.audioReplacement()) {
            EXPECT_EQ(actual.audioReplacement()->path, expected.audioReplacement()->path);
            EXPECT_EQ(actual.audioReplacement()->offset, expected.audioReplacement()->offset);
            EXPECT_EQ(actual.audioReplacement()->duration, expected.audioReplacement()->duration);
        }
    }

    // Le stesse tracce audio (gli id possono cambiare), con i tempi esatti.
    static void expectSameAudio(const AudioTracks& expected, const AudioTracks& actual)
    {
        EXPECT_EQ(actual.mix(AudioTracks::kOriginalTrack), expected.mix(AudioTracks::kOriginalTrack));
        ASSERT_EQ(actual.tracks().size(), expected.tracks().size());
        for (std::size_t t = 0; t < expected.tracks().size(); ++t) {
            const AudioTrack& te = expected.tracks()[t];
            const AudioTrack& ta = actual.tracks()[t];
            EXPECT_EQ(ta.mix, te.mix) << "traccia " << t;
            ASSERT_EQ(ta.clips.size(), te.clips.size()) << "traccia " << t;
            for (std::size_t c = 0; c < te.clips.size(); ++c) {
                const auto exact = [](double s) { return ticksToSeconds(secondsToTicks(s)); };
                EXPECT_EQ(ta.clips[c].path, te.clips[c].path);
                EXPECT_EQ(ta.clips[c].start, exact(te.clips[c].start));
                EXPECT_EQ(ta.clips[c].end, exact(te.clips[c].end));
                EXPECT_EQ(ta.clips[c].position, exact(te.clips[c].position));
                EXPECT_EQ(ta.clips[c].sourceDuration, exact(te.clips[c].sourceDuration));
            }
        }
    }

    fs::path dir_;
};

TEST_F(ProjectFileTest, SaveAndReloadGiveTheSameProject)
{
    ProjectView view;
    const Project project = sample(view);
    saveProjectFile(projectPath(), project, view);

    const LoadedProject loaded = loadProjectFile(projectPath());
    EXPECT_TRUE(loaded.missing.empty());
    expectSame(project, loaded.project);
    EXPECT_EQ(loaded.project.sequence().mutedRanges().size(), project.sequence().mutedRanges().size());
    // La Sorgente mostrava il secondo file: dopo la riapertura è sempre il secondo (con il suo nuovo id).
    EXPECT_EQ(loaded.project.indexOf(loaded.view.sourceClipId), 1);
    EXPECT_EQ(loaded.view.in, view.in);
    EXPECT_EQ(loaded.view.out, view.out);
    EXPECT_EQ(loaded.view.sourcePosition, view.sourcePosition);
    EXPECT_EQ(loaded.view.programPosition, view.programPosition);
    ASSERT_EQ(loaded.savedDurations.size(), 2u);
    EXPECT_EQ(loaded.savedDurations.at(loaded.project.clips()[1].id), 12.48);

    // Salvando di nuovo il progetto riaperto (con i file analizzati di nuovo) si ottiene lo stesso testo.
    const std::string first = read(projectPath());
    Project reopened = loaded.project;
    for (const auto& [id, duration] : loaded.savedDurations)
        reopened.setAnalysis(id, {}, duration);
    const std::string again = utf8FromPath(dir_ / "progetto" / "di_nuovo.evproj");
    saveProjectFile(again, reopened, {loaded.project.clips()[1].id, view.in, view.out, view.sourcePosition,
                                            view.programPosition});
    EXPECT_EQ(read(again), first);

    // Gli originali non sono stati toccati, e il file è JSON leggibile.
    EXPECT_EQ(read(dir_ / "progetto" / "musica.mp3"), "contenuto originale");
    EXPECT_NE(first.find("\"formato\": \"EditorVideo progetto\""), std::string::npos);
    EXPECT_NE(first.find("\"relativo\": \"video\\\\gita.mp4\""), std::string::npos) << first;
    EXPECT_FALSE(fs::exists(dir_ / "progetto" / "montaggio.evproj.tmp"));
}

TEST_F(ProjectFileTest, MovedFolderIsFoundThroughRelativePaths)
{
    ProjectView view;
    saveProjectFile(projectPath(), sample(view), view);
    fs::rename(dir_ / "progetto", dir_ / "spostato");

    const LoadedProject loaded = loadProjectFile(utf8FromPath(dir_ / "spostato" / "montaggio.evproj"));
    EXPECT_TRUE(loaded.missing.empty());
    ASSERT_EQ(loaded.project.clips().size(), 2u);
    EXPECT_EQ(loaded.project.clips()[0].path, utf8FromPath(dir_ / "spostato" / "video" / "gita.mp4"));
    EXPECT_EQ(loaded.project.audioReplacement()->path, utf8FromPath(dir_ / "spostato" / "musica.mp3"));
    EXPECT_EQ(loaded.project.sequence().fragments().size(), 4u);
}

TEST_F(ProjectFileTest, MissingFilesAreListedAndCanBeDroppedOrFound)
{
    ProjectView view;
    saveProjectFile(projectPath(), sample(view), view);
    fs::rename(dir_ / "progetto" / "video" / fs::path(u8"città.mkv"), dir_ / "altrove.mkv");
    fs::remove(dir_ / "progetto" / "musica.mp3");

    LoadedProject loaded = loadProjectFile(projectPath());
    ASSERT_EQ(loaded.missing.size(), 2u);
    EXPECT_EQ(loaded.missing[0].fileName, "città.mkv");
    EXPECT_EQ(loaded.missing[1].clipId, 0);  // l'audio sostitutivo
    EXPECT_EQ(loaded.project.sequence().fragments().size(), 4u);  // finché non si decide, i pezzi restano

    // Trovato altrove il primo, senza il secondo.
    LoadedProject found = loaded;
    relocateMissing(found, found.missing[0], utf8FromPath(dir_ / "altrove.mkv"));
    ASSERT_EQ(found.missing.size(), 1u);
    EXPECT_EQ(found.project.clips()[1].path, utf8FromPath(dir_ / "altrove.mkv"));
    EXPECT_EQ(dropMissing(found), 0);
    EXPECT_FALSE(found.project.audioReplacement().has_value());
    EXPECT_EQ(found.project.sequence().fragments().size(), 4u);

    // Senza nessuno dei due: tolti i due pezzi di quel file.
    EXPECT_EQ(dropMissing(loaded), 2);
    EXPECT_EQ(loaded.project.clips().size(), 1u);
    EXPECT_EQ(loaded.project.sequence().fragments().size(), 2u);
    EXPECT_EQ(loaded.view.sourceClipId, 0);  // la Sorgente mostrava proprio quel file
}

TEST_F(ProjectFileTest, NewerFormatIsRefused)
{
    ProjectView view;
    std::string json = projectToJson(projectPath(), sample(view), view);
    const auto at = json.find("\"versione\": 1");
    ASSERT_NE(at, std::string::npos);
    json.replace(at, 13, "\"versione\": 7");
    try {
        projectFromJson(projectPath(), json);
        ADD_FAILURE() << "doveva essere rifiutato";
    } catch (const ProjectFileError& e) {
        EXPECT_NE(std::string(e.what()).find("versione più recente"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("formato 7"), std::string::npos) << e.what();
    }
}

TEST_F(ProjectFileTest, DamagedFilesAreRefusedWithoutCrashing)
{
    ProjectView view;
    const std::string json = projectToJson(projectPath(), sample(view), view);
    const auto refused = [&](const std::string& text, const char* expected) {
        try {
            projectFromJson(projectPath(), text);
            ADD_FAILURE() << "doveva essere rifiutato: " << text.substr(0, 80);
        } catch (const ProjectFileError& e) {
            EXPECT_NE(std::string(e.what()).find(expected), std::string::npos) << e.what();
        }
    };
    refused(json.substr(0, json.size() / 2), "rovinato");                        // troncato
    refused("questo non è JSON", "rovinato");
    refused("[1, 2, 3]", "non è un progetto");
    refused(R"({"formato": "altro programma", "versione": 1})", "non è un progetto");
    refused(R"({"formato": "EditorVideo progetto"})", "versione");
    // Tipi sbagliati, pezzi che puntano a file inesistenti, tempi al contrario.
    refused(R"({"formato": "EditorVideo progetto", "versione": 1, "file": "x", "sequenza": []})", "non è un progetto");
    refused(R"({"formato": "EditorVideo progetto", "versione": 1, "file": [], "sequenza": [{"file": 3, "inizio": 0, "fine": 1}]})",
            "file che non c'è");
    refused(R"({"formato": "EditorVideo progetto", "versione": 1, "file": [{"id": 1, "percorso": "a.mp4"}],
                "sequenza": [{"file": 1, "inizio": 5, "fine": 1}]})",
            "finisce prima");
    refused(R"({"formato": "EditorVideo progetto", "versione": 1, "file": [{"id": 1, "percorso": "a.mp4"}],
                "sequenza": [{"file": 1, "inizio": "zero", "fine": 1}]})",
            "inizio");

    // Un file vuoto o inesistente.
    write(dir_ / "vuoto.evproj", "");
    EXPECT_THROW(loadProjectFile(utf8FromPath(dir_ / "vuoto.evproj")), ProjectFileError);
    EXPECT_THROW(loadProjectFile(utf8FromPath(dir_ / "non_esiste.evproj")), ProjectFileError);
}

TEST_F(ProjectFileTest, GapsAreSavedAndOldProjectsHaveNone)
{
    ProjectView view;
    Project project = sample(view);
    const std::string plain = projectToJson(projectPath(), project, view);
    EXPECT_NE(plain.find("\"versione\": 1"), std::string::npos);  // senza spazi: come i progetti di prima
    EXPECT_EQ(plain.find("spazioPrima"), std::string::npos);

    Sequence& s = project.sequence();
    ASSERT_TRUE(s.moveTo(s.fragments()[3].id, s.startOf(3) + 1.25));  // l'ultimo più avanti...
    ASSERT_TRUE(s.moveTo(s.fragments()[2].id, s.startOf(2) + 1.0));   // ...e il penultimo nello spazio lasciato
    ASSERT_EQ(s.gapCount(), 2);
    saveProjectFile(projectPath(), project, view);
    const std::string text = read(projectPath());
    EXPECT_NE(text.find("\"versione\": 2"), std::string::npos);
    EXPECT_NE(text.find("spazioPrima"), std::string::npos);

    const LoadedProject loaded = loadProjectFile(projectPath());
    expectSame(project, loaded.project);
    EXPECT_EQ(loaded.project.sequence().gapCount(), 2);
    EXPECT_NEAR(loaded.project.sequence().totalLength(), project.sequence().totalLength(), 1e-6);
}

TEST_F(ProjectFileTest, ProxyQualityIsSavedForEachFile)
{
    ProjectView view;
    const Project project = sample(view);
    const std::string plain = projectToJson(projectPath(), project, view);
    EXPECT_EQ(plain.find("\"proxy\""), std::string::npos);  // nessun proxy: niente di nuovo nel file

    view.proxyDivisors[project.clips()[1].id] = 4;  // solo il secondo file
    saveProjectFile(projectPath(), project, view);
    const LoadedProject loaded = loadProjectFile(projectPath());
    ASSERT_EQ(loaded.view.proxyDivisors.size(), 1u);
    EXPECT_EQ(loaded.view.proxyDivisors.at(loaded.project.clips()[1].id), 4);

    // Un valore sconosciuto si ignora: anteprima dall'originale.
    std::string text = read(projectPath());
    const std::string saved = "\"proxy\": 4";
    const auto at = text.find(saved);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, saved.size(), "\"proxy\": 3");
    EXPECT_TRUE(projectFromJson(projectPath(), text).view.proxyDivisors.empty());
}

// Un progetto di prima, con la traccia sostitutiva, si apre come clip su A2 con A1 silenziata; salvato di nuovo resta
// nel formato di prima, e riaperto è identico.
TEST_F(ProjectFileTest, OldProjectWithReplacementOpensSavesAndReopensTheSame)
{
    const std::string old = R"({
  "formato": "EditorVideo progetto",
  "versione": 1,
  "file": [{"id": 4, "percorso": "", "relativo": "video/gita.mp4", "durata": 7056000000}],
  "sequenza": [{"file": 4, "inizio": 0, "fine": 3528000000, "senzaAudio": []}],
  "audioSostitutivo": {"percorso": "", "relativo": "musica.mp3", "scostamento": -882000000, "durata": 127008000000},
  "sorgente": {"file": 4, "in": null, "out": null, "posizione": 0},
  "montaggio": {"posizione": 0}
})";
    const LoadedProject loaded = projectFromJson(projectPath(), old);
    EXPECT_TRUE(loaded.missing.empty());
    const AudioTracks& audio = loaded.project.audioTracks();
    EXPECT_TRUE(audio.mix(AudioTracks::kOriginalTrack).muted);  // A1 silenziata
    ASSERT_EQ(audio.tracks().size(), 1u);
    ASSERT_EQ(audio.tracks()[0].clips.size(), 1u);
    const AudioClip& clip = audio.tracks()[0].clips[0];
    EXPECT_EQ(clip.path, file("musica.mp3"));
    EXPECT_EQ(clip.start, 1.25);  // scostamento -1,25 s: se ne salta l'inizio
    EXPECT_EQ(clip.position, 0.0);
    EXPECT_EQ(clip.end, 180.0);
    ASSERT_TRUE(loaded.project.audioReplacement().has_value());
    EXPECT_EQ(loaded.project.audioReplacement()->offset, -1.25);

    saveProjectFile(projectPath(), loaded.project, loaded.view);
    const std::string saved = read(projectPath());
    EXPECT_NE(saved.find("\"versione\": 1"), std::string::npos);
    EXPECT_NE(saved.find("\"scostamento\": -882000000"), std::string::npos) << saved;
    EXPECT_EQ(saved.find("tracceAudio"), std::string::npos);

    const LoadedProject again = loadProjectFile(projectPath());
    expectSame(loaded.project, again.project);
    saveProjectFile(projectPath(), again.project, again.view);
    EXPECT_EQ(read(projectPath()), saved);
}

TEST_F(ProjectFileTest, AudioTracksAreSavedWithLevelsMuteAndSolo)
{
    ProjectView view;
    Project project = sample(view);
    project.setAudioReplacement(std::nullopt);
    AudioTracks& audio = project.audioTracks();
    audio.setLevel(AudioTracks::kOriginalTrack, -3.5);
    const int a2 = audio.addTrack();
    const int a3 = audio.addTrack();
    audio.setLevel(a2, -12.25);
    audio.setSolo(a2, true);
    audio.setMuted(a3, true);
    ASSERT_NE(audio.add(a2, AudioClip{0, file("voce.wav"), 0.5, 2.0 + 1.0 / 48000, 1.0, 9.5}), 0);
    ASSERT_NE(audio.add(a2, AudioClip{0, file("musica.mp3"), 10.0, 70.0, 4.0, 181.3}), 0);
    ASSERT_NE(audio.add(a3, AudioClip{0, file("effetto.wav"), 0.0, 1001.0 / 30000, 2.5, 0.0}), 0);
    ASSERT_FALSE(project.audioReplacement().has_value());
    ASSERT_TRUE(audio.setClipEnvelope(audio.tracks()[0].clips[0].id, {{0.5, -6.5}, {1.25, 3.0}, {2.0, 3.0}}));
    ASSERT_TRUE(project.sequence().setEnvelope(project.sequence().fragments()[0].id, {{0.0, 0.0}, {0.5, -60.0}}));

    saveProjectFile(projectPath(), project, view);
    const std::string text = read(projectPath());
    EXPECT_NE(text.find("\"versione\": 3"), std::string::npos);
    EXPECT_NE(text.find("\"tracceAudio\""), std::string::npos);
    EXPECT_NE(text.find("\"livello\": -12.25"), std::string::npos) << text;
    EXPECT_NE(text.find("\"linea\""), std::string::npos) << text;
    EXPECT_EQ(text.find("calamita"), std::string::npos) << text;

    const LoadedProject loaded = loadProjectFile(projectPath());
    EXPECT_TRUE(loaded.missing.empty());
    expectSame(project, loaded.project);
    EXPECT_FALSE(loaded.project.audioTracks().audible(loaded.project.audioTracks().tracks()[1].id));
    EXPECT_EQ(loaded.project.audioTracks().tracks()[0].clips[0].envelope.size(), 3u);
    EXPECT_EQ(loaded.project.sequence().fragments()[0].envelope.size(), 2u);

    // Salvato di nuovo (con i file analizzati di nuovo): lo stesso testo.
    Project reopened = loaded.project;
    for (const auto& [id, duration] : loaded.savedDurations)
        reopened.setAnalysis(id, {}, duration);
    saveProjectFile(projectPath(), reopened, loaded.view);
    EXPECT_EQ(read(projectPath()), text);

    // Una clip audio rovinata si rifiuta.
    std::string damaged = text;
    const std::string end = "\"fine\": " + std::to_string(secondsToTicks(70.0));
    const auto at = damaged.find(end);
    ASSERT_NE(at, std::string::npos) << text;
    damaged.replace(at, end.size(), "\"fine\": 0");
    EXPECT_THROW(projectFromJson(projectPath(), damaged), ProjectFileError);
}

TEST_F(ProjectFileTest, FreeClipsOnTheA1LaneAreSaved)
{
    ProjectView view;
    Project project = sample(view);
    project.setAudioReplacement(std::nullopt);
    AudioTracks& audio = project.audioTracks();
    const int a2 = audio.addTrack();
    ASSERT_NE(audio.add(a2, AudioClip{0, file("musica.mp3"), 0.0, 5.0, 1.0, 181.3}), 0);
    ASSERT_NE(audio.add(AudioTracks::kOriginalTrack, AudioClip{0, file("voce.wav"), 0.5, 2.0, 1.0, 9.5}), 0);
    ASSERT_TRUE(audio.hasHost());
    saveProjectFile(projectPath(), project, view);
    const std::string text = read(projectPath());
    EXPECT_NE(text.find("\"suA1\": true"), std::string::npos) << text;
    const LoadedProject loaded = loadProjectFile(projectPath());
    const AudioTracks& again = loaded.project.audioTracks();
    ASSERT_TRUE(again.hasHost());
    ASSERT_EQ(again.tracks().size(), 2u);
    EXPECT_EQ(again.tracks()[0].id, AudioTracks::kOriginalTrack);
    EXPECT_EQ(again.tracks()[0].clips.size(), 1u);
    EXPECT_EQ(again.trackName(again.tracks()[1].id), "A2");
    expectSame(project, loaded.project);
}

TEST_F(ProjectFileTest, MissingAudioFilesOfTheTracks)
{
    ProjectView view;
    Project project = sample(view);
    project.setAudioReplacement(std::nullopt);
    AudioTracks& audio = project.audioTracks();
    const int a2 = audio.addTrack();
    audio.add(a2, AudioClip{0, file("voce.wav"), 0.0, 1.0, 0.0, 9.5});
    audio.add(a2, AudioClip{0, file("voce.wav"), 3.0, 4.0, 2.0, 9.5});
    audio.add(a2, AudioClip{0, file("effetto.wav"), 0.0, 1.0, 5.0, 1.0});
    saveProjectFile(projectPath(), project, view);
    fs::rename(dir_ / "progetto" / "voce.wav", dir_ / "voce.wav");

    LoadedProject loaded = loadProjectFile(projectPath());
    ASSERT_EQ(loaded.missing.size(), 1u);  // una volta sola, anche se lo usano due clip
    EXPECT_EQ(loaded.missing[0].clipId, 0);
    EXPECT_EQ(loaded.missing[0].fileName, "voce.wav");

    LoadedProject found = loaded;
    relocateMissing(found, found.missing[0], utf8FromPath(dir_ / "voce.wav"));
    EXPECT_TRUE(found.missing.empty());
    EXPECT_EQ(found.project.audioTracks().tracks()[0].clips[1].path, utf8FromPath(dir_ / "voce.wav"));

    EXPECT_EQ(dropMissing(loaded), 0);  // nessun pezzo video tolto
    ASSERT_EQ(loaded.project.audioTracks().tracks()[0].clips.size(), 1u);
    EXPECT_EQ(loaded.project.audioTracks().tracks()[0].clips[0].path, file("effetto.wav"));
}

TEST(GapExport, GapsAreClosedInTheExportedFile)
{
    Project project;
    const int a = project.add("C:\\video\\a.mp4");
    project.setAnalysis(a, {0.0, 1.0, 2.0, 3.0}, 4.0);
    Sequence& s = project.sequence();
    s.insert(-1, a, 0.0, 1.0);
    const int second = s.insert(-1, a, 2.0, 4.0);
    ASSERT_TRUE(s.moveTo(second, 3.0));  // spazio 1 .. 3
    s.muteRange(3.5, 4.0);               // nel secondo pezzo, nei tempi della sequenza

    const auto plan = project.exportPlan();
    ASSERT_EQ(plan.size(), 1u);
    ASSERT_EQ(plan[0].ranges.size(), 2u);
    // Nel file esportato i pezzi sono attaccati: il tratto senza audio è 1,5 .. 2 (non 3,5 .. 4).
    const AudioEdit audio = project.audioEdit();
    ASSERT_EQ(audio.muted.size(), 1u);
    EXPECT_NEAR(audio.muted[0].start, 1.5, 1e-9);
    EXPECT_NEAR(audio.muted[0].end, 2.0, 1e-9);
    EXPECT_EQ(project.sequence().gapCount(), 1);  // il montaggio resta com'è
}

}  // namespace
