#include "EditorApp/SelfTests.h"

#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TimelineWidget.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Thumbnails.h"
#include "EditorCore/TimeUtil.h"
#include "EditorCore/VideoDecoder.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>

namespace editor::app {

namespace {

constexpr int kSmokePlayMilliseconds = 2000;

namespace core = editor::core;

// Esegue una prova: un'eccezione (file che non si apre, esportazione rifiutata...) vuol dire codice 2.
int guarded(const std::function<int()>& test)
{
    try {
        return test();
    } catch (const std::exception& e) {
        std::printf("Errore: %s\n", e.what());
        return 2;
    }
}

struct AnalyzedClip
{
    int id = 0;
    core::MediaInfo info;
};

// Analizza il file e lo aggiunge al progetto con durata e fotogrammi chiave (come fa l'interfaccia dopo l'analisi).
AnalyzedClip addAnalyzedClip(core::Project& project, const std::string& path)
{
    AnalyzedClip clip;
    clip.info = core::probeFile(path);
    clip.id = project.add(path);
    project.setAnalysis(clip.id, clip.info.keyframeTimes, clip.info.durationSeconds);
    return clip;
}

// --join-test <destinazione> <file1> <file2> ...: unisce i file interi come farebbe l'interfaccia (dopo il controllo
// di compatibilità) e controlla il file creato. Codice di uscita 0 se è tutto giusto, 4 se i file non si possono
// unire (il motivo viene scritto), 2 per altri errori, 3 se il controllo del risultato non riesce.
int runJoinTest(const QStringList& args, qsizetype at)
{
    if (at + 3 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --join-test <destinazione> <file1> <file2> [...]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    return guarded([&]() -> int {
        core::Project project;
        std::vector<core::MediaSignature> signatures;
        std::vector<core::MediaInfo> sources;
        for (qsizetype i = at + 2; i < args.size(); ++i) {
            const std::string path = args[i].toStdString();
            const AnalyzedClip clip = addAnalyzedClip(project, path);
            sources.push_back(clip.info);
            signatures.push_back(core::readMediaSignature(path));
            project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds, project.snapper());  // file intero
        }
        if (const auto mismatches = core::checkCompatibility(signatures); !mismatches.empty()) {
            std::printf("Non si possono unire:\n%s\n", core::describeMismatches(mismatches).c_str());
            return 4;
        }
        const core::ExportResult result = core::exportClips(project.exportPlan(), output, core::AudioEdit{});
        const core::MediaInfo joined = core::probeFile(output);
        const double expected = project.sequence().totalLength();
        std::printf("Uniti %d file: durata attesa %.3f s, ottenuta %.3f s, %d flussi\n", result.clipCount, expected,
                    joined.durationSeconds, result.streamCount);

        const double tolerance = 0.1 + 0.05 * static_cast<double>(result.clipCount);
        bool ok = std::abs(joined.durationSeconds - expected) < tolerance
               && joined.streams.size() == static_cast<std::size_t>(result.streamCount);
        for (std::size_t s = 0; ok && s < joined.streams.size() && s < sources.front().streams.size(); ++s)
            ok = joined.streams[s].codec == sources.front().streams[s].codec;
        if (joined.keyframeStreamIndex >= 0)
            ok = ok && !joined.keyframeTimes.empty() && std::abs(joined.keyframeTimes.front()) < 0.001;
        std::printf("%s\n", ok ? "Controllo del file unito: OK" : "Controllo del file unito: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// Luminosità media di un fotogramma (per riconoscere da quale punto di quale file arriva).
double meanBrightness(const std::string& file, double seconds)
{
    core::VideoDecoder video(file, {.allowHardware = false});
    const auto frame = video.seek(seconds);
    if (!frame)
        return -1.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < frame->pixels.size(); i += 4)
        sum += (frame->pixels[i] + frame->pixels[i + 1] + frame->pixels[i + 2]) / 3.0;
    return sum / (frame->pixels.size() / 4.0);
}

// --sequence-test <destinazione> <file> <IN s> <OUT s> [<file> <IN s> <OUT s> ...]: costruisce un montaggio con i pezzi
// in quell'ordine (lo stesso file può tornare più volte), lo esporta e controlla durata e ordine: all'inizio di ogni
// pezzo il file creato deve mostrare lo stesso fotogramma del file di origine. Codici come --join-test.
int runSequenceTest(const QStringList& args, qsizetype at)
{
    if (at + 4 >= args.size() || (args.size() - at - 2) % 3 != 0) {
        std::printf("Uso: EditorVideo.exe --sequence-test <destinazione> <file> <IN> <OUT> [<file> <IN> <OUT> ...]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    return guarded([&]() -> int {
        core::Project project;
        std::map<std::string, int> ids;
        std::vector<core::MediaSignature> signatures;
        for (qsizetype i = at + 2; i + 2 < args.size(); i += 3) {
            const std::string path = args[i].toStdString();
            if (!ids.contains(path)) {
                ids[path] = addAnalyzedClip(project, path).id;
                signatures.push_back(core::readMediaSignature(path));
            }
            if (project.sequence().insert(-1, ids[path], args[i + 1].toDouble(), args[i + 2].toDouble(), project.snapper()) == 0)
                throw core::MediaError("Pezzo vuoto: OUT deve stare dopo IN.");
        }
        if (const auto mismatches = core::checkCompatibility(signatures); !mismatches.empty()) {
            std::printf("Non si possono unire:\n%s\n", core::describeMismatches(mismatches).c_str());
            return 4;
        }
        const core::Sequence& sequence = project.sequence();
        const core::ExportResult result = core::exportClips(project.exportPlan(), output, core::AudioEdit{});
        const double length = core::probeFile(output).durationSeconds;
        std::printf("Montaggio di %d pezzi: durata attesa %.3f s, ottenuta %.3f s\n", result.segmentCount,
                    sequence.totalLength(), length);
        bool ok = std::abs(length - sequence.totalLength()) < 0.1 + 0.03 * static_cast<double>(result.segmentCount);

        // Ordine: subito dopo l'inizio di ogni pezzo, stesso fotogramma dell'originale.
        double start = 0.0;  // inizio del pezzo nel montaggio
        for (std::size_t k = 0; k < sequence.fragments().size(); ++k) {
            const core::Fragment& f = sequence.fragments()[k];
            const double inOutput = meanBrightness(output, start + 0.01);
            start += f.length();
            const double inSource = meanBrightness(project.find(f.clipId)->path, f.start + 0.01);
            const bool same = inOutput >= 0 && std::abs(inOutput - inSource) < 3.0;
            std::printf("  pezzo %d (%.3f - %.3f s): luminosità %.1f, originale %.1f%s\n", static_cast<int>(k) + 1,
                        f.start, f.end, inOutput, inSource, same ? "" : "  <-- DIVERSO");
            ok = ok && same;
        }
        std::printf("%s\n", ok ? "Controllo del montaggio: OK" : "Controllo del montaggio: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --codec-info <file> [<file> ...]: scrive i parametri dei flussi (per H.264 anche SPS e PPS letti) e, con più file,
// se si possono unire e perché. Serve a capire un rifiuto di unione.
int runCodecInfo(const QStringList& args, qsizetype at)
{
    std::vector<core::MediaSignature> signatures;
    return guarded([&]() -> int {
        for (qsizetype i = at + 1; i < args.size(); ++i) {
            const core::MediaSignature s = core::readMediaSignature(args[i].toStdString());
            std::printf("== %s\n", s.fileName.c_str());
            for (const core::StreamSignature& stream : s.streams) {
                if (stream.kind == core::StreamKind::Video) {
                    std::printf("video %s %s %dx%d %s %.3f fps, dati extra %d byte\n", stream.codec.c_str(),
                                stream.profile.c_str(), stream.width, stream.height, stream.pixelFormat.c_str(),
                                stream.frameRate, static_cast<int>(stream.extradata.size()));
                    if (stream.codec == "h264")
                        std::printf("%s", core::describeH264(core::parseH264Config(stream.extradata)).c_str());
                } else {
                    std::printf("audio %s %s %d Hz %s, dati extra %d byte\n", stream.codec.c_str(), stream.profile.c_str(),
                                stream.sampleRate, stream.channelLayout.c_str(), static_cast<int>(stream.extradata.size()));
                }
            }
            signatures.push_back(s);
        }
        if (signatures.size() > 1) {
            const auto mismatches = core::checkCompatibility(signatures);
            std::printf("%s\n", mismatches.empty() ? "Si possono unire."
                                                   : ("Non si possono unire:\n" + core::describeMismatches(mismatches)).c_str());
        }
        return 0;
    });
}

// Legge tutto l'audio (`onChunk` vede ogni blocco) e restituisce il buco più lungo fra due blocchi decodificati di
// seguito (alcuni registratori ne lasciano già qualcuno).
double readLongestGap(core::AudioDecoder& audio,
                      const std::function<void(const core::AudioChunk&)>& onChunk = {})
{
    double previousEnd = -1.0;
    double longest = 0.0;
    while (auto chunk = audio.next()) {
        if (previousEnd >= 0)
            longest = std::max(longest, chunk->time - previousEnd);
        previousEnd = chunk->time + chunk->samples.size() / static_cast<double>(audio.channels()) / audio.sampleRate();
        if (onChunk)
            onChunk(*chunk);
    }
    return longest;
}

double longestAudioGap(const std::string& file)
{
    core::AudioDecoder audio(file);
    return readLongestGap(audio);
}

// --audio-test <destinazione> <file> [mute <da s> <a s>]... [replace <audio> <offset s>] [omit]: mette il file intero nel
// montaggio, toglie l'audio dai tratti indicati e/o lo sostituisce, esporta e controlla il file creato: durata,
// codec, audio continuo (nessun buco fra un blocco e l'altro) e silenzio nei tratti. "omit" toglie i pacchetti invece
// di metterci il silenzio (per confronto). Codici come --join-test (4 = audio sostitutivo non utilizzabile).
int runAudioTest(const QStringList& args, qsizetype at)
{
    if (at + 2 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --audio-test <destinazione> <file> [mute <da> <a>]... [replace <audio> <offset>] [omit]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    const std::string input = args[at + 2].toStdString();
    return guarded([&]() -> int {
        core::Project project;
        const AnalyzedClip clip = addAnalyzedClip(project, input);
        project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds, project.snapper());
        core::MuteMethod method = core::MuteMethod::Silence;
        for (qsizetype i = at + 3; i < args.size(); ++i) {
            if (args[i] == QStringLiteral("mute") && i + 2 < args.size()) {
                project.sequence().muteRange(args[i + 1].toDouble(), args[i + 2].toDouble());
                i += 2;
            } else if (args[i] == QStringLiteral("replace") && i + 2 < args.size()) {
                const std::string audio = args[i + 1].toStdString();
                if (const std::string problem = core::replacementAudioProblem(audio, output); !problem.empty()) {
                    std::printf("Audio sostitutivo non utilizzabile: %s\n", problem.c_str());
                    return 4;
                }
                project.setAudioReplacement(core::AudioReplacement{audio, args[i + 2].toDouble(), core::AudioDecoder(audio).durationSeconds()});
                i += 2;
            } else if (args[i] == QStringLiteral("omit")) {
                method = core::MuteMethod::Omit;
            }
        }
        const core::AudioEdit edit = project.audioEdit();
        const core::ExportResult result = core::exportClips(project.exportPlan(), output, edit, {}, method);
        const core::MediaInfo exported = core::probeFile(output);
        const double expected = project.sequence().totalLength();
        std::printf("Esportato: durata attesa %.3f s, ottenuta %.3f s; audio: %d pacchetti col silenzio, %d tolti\n",
                    expected, exported.durationSeconds, result.silencedPackets, result.droppedPackets);
        bool ok = std::abs(exported.durationSeconds - expected) < 0.1;

        // Audio del file creato: codec, continuità e livello dentro e fuori dai tratti silenziati.
        std::string audioCodec;
        for (const core::StreamInfo& stream : exported.streams) {
            if (stream.kind == core::StreamKind::Audio && audioCodec.empty())
                audioCodec = stream.codec;
        }
        if (edit.replacement) {
            const core::MediaInfo source = core::probeFile(edit.replacement->path);
            for (const core::StreamInfo& stream : source.streams) {
                if (stream.kind == core::StreamKind::Audio) {
                    std::printf("Audio sostitutivo: %s, nel file creato: %s\n", stream.codec.c_str(), audioCodec.c_str());
                    ok = ok && stream.codec == audioCodec;
                    break;
                }
            }
        }
        if (!audioCodec.empty()) {
            core::AudioDecoder audio(output);
            const int channels = audio.channels();
            const double rate = audio.sampleRate();
            double mutedPeak = 0.0;
            double otherPeak = 0.0;
            const double longestGap = readLongestGap(audio, [&](const core::AudioChunk& chunk) {
                constexpr double kMargin = 0.05;  // ai bordi di un tratto un blocco AAC può essere a metà
                const std::size_t frames = chunk.samples.size() / channels;
                for (std::size_t n = 0; n < frames; ++n) {
                    const double t = chunk.time + n / rate;
                    const double level = std::abs(chunk.samples[n * channels]) / 32768.0;
                    bool inside = false;
                    bool near = false;
                    for (const core::TimeRange& m : edit.muted) {
                        inside = inside || (t >= m.start + kMargin && t < m.end - kMargin);
                        near = near || (t >= m.start - kMargin && t < m.end + kMargin);
                    }
                    if (inside)
                        mutedPeak = std::max(mutedPeak, level);
                    else if (!near)
                        otherPeak = std::max(otherPeak, level);
                }
            });
            // I buchi già presenti nell'audio di partenza restano (si copia senza ricodificare): non devono essercene
            // di nuovi.
            const double sourceGap = longestAudioGap(edit.replacement ? edit.replacement->path : input);
            std::printf("Audio %s: buco più lungo fra due blocchi %.1f ms (nell'audio di partenza %.1f ms); picco nei "
                        "tratti senza audio %.4f, altrove %.4f\n",
                        audioCodec.c_str(), longestGap * 1000, sourceGap * 1000, mutedPeak, otherPeak);
            ok = ok && (method == core::MuteMethod::Omit || longestGap < std::max(0.03, sourceGap + 0.005))
              && mutedPeak < 0.005;
        }
        std::printf("%s\n", ok ? "Controllo dell'audio: OK" : "Controllo dell'audio: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --project-test <file> [<file> ...]: prova il file di progetto con file veri, in una cartella temporanea. Copia i file
// in una cartella, costruisce un montaggio (con audio tolto e sostituito), lo salva e lo riapre (deve tornare
// identico), sposta la cartella (i file si devono ritrovare dal percorso relativo) ed esporta il progetto riaperto;
// poi toglie un file (deve risultare mancante) e prova un progetto di versione sconosciuta e uno rovinato (rifiutati
// con un messaggio). Codice 0 se è tutto giusto, 3 se un controllo non riesce, 2 per altri errori.
int runProjectTest(const QStringList& args, qsizetype at)
{
    namespace fs = std::filesystem;
    if (at + 1 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --project-test <file> [<file> ...]\n");
        return 1;
    }
    const auto projectFileName = [](const std::string& name) { return name + core::kProjectExtension; };
    const auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    bool ok = true;
    const auto check = [&ok](bool condition, const char* what) {
        std::printf("  %s: %s\n", what, condition ? "OK" : "NON RIUSCITO");
        ok = ok && condition;
    };
    const fs::path base = fs::temp_directory_path() / "EditorVideoProvaProgetto";
    return guarded([&]() -> int {
        fs::remove_all(base);
        fs::create_directories(base / "originale" / "video");

        // Montaggio: per ogni file il file intero e poi la sua prima metà; audio tolto in un tratto; il primo file come
        // audio sostitutivo, spostato di mezzo secondo.
        core::Project project;
        for (qsizetype i = at + 1; i < args.size(); ++i) {
            const fs::path source = fs::path(args[i].toStdWString());
            const fs::path copy = base / "originale" / "video" / source.filename();
            fs::copy_file(source, copy, fs::copy_options::overwrite_existing);
            const AnalyzedClip clip = addAnalyzedClip(project, core::utf8FromPath(copy));
            project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds, project.snapper());
            project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds / 2, project.snapper());
        }
        const double total = project.sequence().totalLength();
        project.sequence().muteRange(total * 0.2, total * 0.3);
        project.setAudioReplacement(core::AudioReplacement{project.clips().front().path, 0.5, project.clips().front().duration});
        const core::ProjectView view{project.clips().front().id, 0.0, 1.0, 0.5, total / 2};

        const std::string saved = core::utf8FromPath(base / "originale" / projectFileName("montaggio"));
        core::saveProjectFile(saved, project, view);
        std::printf("Progetto salvato: %s (%d file, %d pezzi)\n", saved.c_str(), static_cast<int>(project.clips().size()),
                    static_cast<int>(project.sequence().fragments().size()));

        // Riaperto e salvato di nuovo (dopo aver rifatto l'analisi): stesso testo.
        core::LoadedProject loaded = core::loadProjectFile(saved);
        check(loaded.missing.empty(), "riaperto, nessun file mancante");
        bool sameDurations = true;
        for (const core::Clip& clip : loaded.project.clips()) {
            const core::MediaInfo info = core::probeFile(clip.path);
            loaded.project.setAnalysis(clip.id, info.keyframeTimes, info.durationSeconds);
            sameDurations = sameDurations && std::abs(loaded.savedDurations[clip.id] - info.durationSeconds) < 1e-9;
        }
        check(sameDurations, "durate dei file uguali a quelle salvate");
        const std::string again = core::utf8FromPath(base / "originale" / projectFileName("di_nuovo"));
        core::saveProjectFile(again, loaded.project, loaded.view);
        check(read(core::pathFromUtf8(saved)) == read(core::pathFromUtf8(again)),
              "salvato di nuovo: identico");
        check(loaded.project.sequence().mutedRanges().size() == project.sequence().mutedRanges().size(), "tratti senza audio");
        check(loaded.view.in == view.in && loaded.view.out == view.out, "IN e OUT della Sorgente");

        // Cartella spostata: i file si ritrovano dal percorso relativo, e il progetto riaperto si esporta.
        fs::rename(base / "originale", base / "spostata");
        core::LoadedProject moved =
            core::loadProjectFile(core::utf8FromPath(base / "spostata" / projectFileName("montaggio")));
        bool inside = moved.missing.empty();
        for (const core::Clip& clip : moved.project.clips()) {
            inside = inside && clip.path.find("spostata") != std::string::npos
                  && fs::exists(core::pathFromUtf8(clip.path));
            const core::MediaInfo info = core::probeFile(clip.path);
            moved.project.setAnalysis(clip.id, info.keyframeTimes, info.durationSeconds);
        }
        check(inside, "cartella spostata: file ritrovati dal percorso relativo");
        const std::string extension = core::pathFromUtf8(moved.project.clips().front().path).extension().string();
        const std::string output = core::utf8FromPath(base / ("esportato" + extension));
        core::exportClips(moved.project.exportPlan(), output, moved.project.audioEdit());
        const double length = core::probeFile(output).durationSeconds;
        std::printf("  esportazione del progetto riaperto: %.3f s (attesi %.3f s)\n", length, total);
        check(std::abs(length - total) < 0.1 + 0.05 * static_cast<double>(moved.project.sequence().fragments().size()),
              "esportazione del progetto riaperto");

        // Un file tolto: risulta mancante; senza di lui i suoi pezzi vengono tolti.
        fs::remove(core::pathFromUtf8(moved.project.clips().front().path));
        core::LoadedProject missing =
            core::loadProjectFile(core::utf8FromPath(base / "spostata" / projectFileName("montaggio")));
        std::printf("  file mancanti: %d\n", static_cast<int>(missing.missing.size()));
        const int dropped = core::dropMissing(missing);
        check(!missing.project.audioReplacement() && dropped == 2, "file mancante segnalato e tolto con i suoi 2 pezzi");

        // Versione sconosciuta e file rovinato: rifiutati con un messaggio.
        std::string json = read(base / "spostata" / projectFileName("montaggio"));
        const auto refused = [&](const std::string& text, const char* what) {
            try {
                core::projectFromJson(core::utf8FromPath(base / projectFileName("x")), text);
                check(false, what);
            } catch (const core::ProjectFileError& e) {
                std::printf("  %s -> \"%s\"\n", what, e.what());
                check(true, what);
            }
        };
        std::string newer = json;
        newer.replace(newer.find("\"versione\": 1"), 13, "\"versione\": 9");
        refused(newer, "versione sconosciuta rifiutata");
        refused(json.substr(0, json.size() / 3), "progetto rovinato rifiutato");

        fs::remove_all(base);
        std::printf("%s\n", ok ? "Controllo del progetto: OK" : "Controllo del progetto: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --thumbnail-test <file> [...]: estrae una miniatura per ogni fotogramma chiave (al massimo 20 per file), come la
// timeline del montaggio, con la GPU se c'è, e scrive quanto ci vuole. Codice 0 se per ogni file con video tutte le
// miniature sono uscite, della misura giusta; 3 altrimenti; 2 per altri errori.
int runThumbnailTest(const QStringList& args, qsizetype at)
{
    bool ok = true;
    return guarded([&]() -> int {
        core::ThumbnailExtractor extractor(192, 108, true);
        for (qsizetype i = at + 1; i < args.size(); ++i) {
            const std::string path = args[i].toStdString();
            const core::MediaInfo info = core::probeFile(path);
            std::vector<double> times = info.keyframeTimes;
            if (times.size() > 20)
                times.resize(20);
            QElapsedTimer timer;
            timer.start();
            int made = 0;
            int width = 0;
            int height = 0;
            for (double t : times) {
                if (const auto frame = extractor.extract(path, t)) {
                    ++made;
                    width = frame->width;
                    height = frame->height;
                }
            }
            const double ms = timer.nsecsElapsed() / 1e6;
            std::printf("%s: %d miniature su %d (%dx%d), %.1f ms ciascuna, decodifica %s\n",
                        QFileInfo(args[i]).fileName().toUtf8().constData(), made, static_cast<int>(times.size()), width,
                        height, times.empty() ? 0.0 : ms / times.size(), extractor.decoderDescription().c_str());
            if (info.keyframeStreamIndex >= 0)
                ok = ok && made == static_cast<int>(times.size()) && width > 0 && width <= 192 && height <= 108;
        }
        std::printf("%s\n", ok ? "Controllo delle miniature: OK" : "Controllo delle miniature: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --export-test <origine> <destinazione> <IN s> <OUT s> [<IN s> <OUT s> ...]: mette i tratti del file nel montaggio,
// nell'ordine dato e ognuno con l'IN agganciato al fotogramma chiave, li esporta come farebbe l'interfaccia e controlla
// il file creato (durata, codec, fotogramma chiave all'inizio). Codici come --join-test.
int runExportTest(const QStringList& args, qsizetype at)
{
    const qsizetype pairs = (args.size() - at - 3) / 2;
    if (at + 4 >= args.size() || (args.size() - at - 3) % 2 != 0) {
        std::printf("Uso: EditorVideo.exe --export-test <origine> <destinazione> <IN s> <OUT s> [<IN s> <OUT s> ...]\n");
        return 1;
    }
    const std::string input = args[at + 1].toStdString();
    const std::string output = args[at + 2].toStdString();
    return guarded([&]() -> int {
        // Un montaggio con i tratti del file, nell'ordine dato: ogni IN si aggancia al fotogramma chiave.
        core::Project project;
        const AnalyzedClip clip = addAnalyzedClip(project, input);
        const core::MediaInfo& source = clip.info;
        const int id = clip.id;
        core::Sequence& sequence = project.sequence();
        for (qsizetype i = 0; i < pairs; ++i) {
            const double in = core::clampTo(args[at + 3 + 2 * i].toDouble(), source.durationSeconds);
            const double out = core::clampTo(args[at + 4 + 2 * i].toDouble(), source.durationSeconds);
            if (sequence.insert(-1, id, in, out, project.snapper()) == 0)
                throw core::MediaError("OUT deve stare dopo IN.");
        }
        for (const core::Fragment& f : sequence.fragments())
            std::printf("Tratto: %.3f - %.3f s\n", f.start, f.end);

        const core::ExportResult result = core::exportClips(project.exportPlan(), output, core::AudioEdit{});
        const core::MediaInfo exported = core::probeFile(output);
        std::printf("Esportato: %d tratti, durata attesa %.3f s, ottenuta %.3f s, %d flussi\n", result.segmentCount,
                    sequence.totalLength(), exported.durationSeconds, result.streamCount);

        // Tolleranza: un paio di fotogrammi per giunzione.
        const double tolerance = 0.1 + 0.05 * static_cast<double>(sequence.fragments().size() - 1);
        bool ok = exported.streams.size() == static_cast<std::size_t>(result.streamCount)
               && std::abs(exported.durationSeconds - sequence.totalLength()) < tolerance;
        // Stessi codec dell'originale, nello stesso ordine (video e audio).
        std::size_t next = 0;
        for (const core::StreamInfo& stream : source.streams) {
            if (stream.kind != core::StreamKind::Video && stream.kind != core::StreamKind::Audio)
                continue;
            if (stream.attachedPicture)
                continue;
            ok = ok && next < exported.streams.size() && exported.streams[next].codec == stream.codec;
            ++next;
        }
        // Il video deve partire con un fotogramma chiave a 0.
        if (exported.keyframeStreamIndex >= 0)
            ok = ok && !exported.keyframeTimes.empty() && std::abs(exported.keyframeTimes.front()) < 0.001;
        std::printf("%s\n", ok ? "Controllo del file esportato: OK" : "Controllo del file esportato: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

struct CommandLineTest
{
    const char* option;
    int (*run)(const QStringList& args, qsizetype at);
};

// Nell'ordine in cui si cercano fra gli argomenti: vale la prima trovata.
constexpr CommandLineTest kTests[] = {
    {"--export-test", runExportTest},   {"--thumbnail-test", runThumbnailTest}, {"--project-test", runProjectTest},
    {"--audio-test", runAudioTest},     {"--codec-info", runCodecInfo},         {"--sequence-test", runSequenceTest},
    {"--join-test", runJoinTest},
};

}  // namespace

std::optional<int> runCommandLineTest(const QStringList& arguments)
{
    for (const CommandLineTest& test : kTests) {
        if (const qsizetype at = arguments.indexOf(QLatin1String(test.option)); at >= 0) {
            const int code = test.run(arguments, at);
            std::fflush(stdout);
            return code;
        }
    }
    return std::nullopt;
}

void startSmokeTest(QApplication& app, MainWindow& window, const QString& file)
{
    std::printf("Piattaforma Qt: %s\n%s\n", qPrintable(QGuiApplication::platformName()),
                qPrintable(versionSummary()));
    std::fflush(stdout);
    if (file.isEmpty()) {
        QTimer::singleShot(0, &app, &QApplication::quit);
    } else {
        PlayerWidget* player = window.player();
        QTimer::singleShot(0, &app, [player] { player->togglePlayPause(); });
        QTimer::singleShot(kSmokePlayMilliseconds, &app, [&app, player, window = &window] {
            const double position = player->currentPosition();
            std::printf("Riproduzione: %s, posizione %.3f s dopo %.1f s, %s\n",
                        player->isPlaying() ? "in corso" : "ferma", position, kSmokePlayMilliseconds / 1000.0,
                        qPrintable(player->statusText()));
            std::fflush(stdout);
            // Codice di uscita 2 se la riproduzione non è partita o non avanza col tempo giusto (±25%).
            const double expected = kSmokePlayMilliseconds / 1000.0;
            bool ok = player->isPlaying() && std::abs(position - expected) < expected * 0.25;

            // Segmento: IN qui (agganciato al fotogramma chiave), 5 fotogrammi avanti, OUT.
            player->markIn();
            for (int i = 0; i < 5; ++i)
                player->stepFrame(+1);
            player->markOut();
            const core::CutSegment& segment = player->segment();
            const auto& keyframes = segment.keyframes();
            const bool inOnKeyframe = keyframes.empty()
                                   || std::any_of(keyframes.begin(), keyframes.end(),
                                                  [&](double k) { return std::abs(k - segment.in()) < 1e-6; });
            std::printf("Segmento: IN %.3f s, OUT %.3f s, durata %.3f s%s\n", segment.in(), segment.out(),
                        segment.length(), inOnKeyframe ? "" : " (IN non su un fotogramma chiave!)");
            std::fflush(stdout);
            // IN non dopo la posizione letta sopra (con margine: la pausa arriva qualche istante dopo).
            ok = ok && segment.hasIn() && segment.hasOut() && inOnKeyframe && segment.in() <= position + 0.1
              && segment.length() > 0;

            const auto finish = [&app, window](bool success) {
                // Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png salva anche un'immagine della finestra.
                const QString screenshot = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
                if (!screenshot.isEmpty()) {
                    QCoreApplication::processEvents();  // applica i cambi di layout ancora in sospeso
                    window->grab().save(screenshot);
                }
                app.exit(success ? 0 : 2);
            };

            // Montaggio: il pezzo appena segnato, poi uno all'inizio dello stesso file (A), poi gli altri file
            // interi.
            window->addToSequence();
            player->seekTo(0.0);
            player->markIn();
            for (int i = 0; i < 3; ++i)
                player->stepFrame(+1);
            player->markOut();
            window->addToSequence();

            // Con più file si aspetta che siano stati analizzati tutti (servono durata e fotogrammi chiave).
            QElapsedTimer analysis;
            analysis.start();
            while (!window->project().allAnalyzed() && analysis.elapsed() < 20000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const int firstClip = window->currentClipId();
            for (const core::Clip& clip : window->project().clips()) {
                if (clip.id != firstClip) {
                    window->selectClip(clip.id);
                    window->addToSequence();  // senza IN e OUT: il file intero
                }
            }
            const core::Sequence& sequence = window->project().sequence();
            const std::size_t expectedPieces = 2 + window->project().clips().size() - 1;
            std::printf("Montaggio: %d pezzi, durata %.3f s\n", static_cast<int>(sequence.fragments().size()),
                        sequence.totalLength());
            std::fflush(stdout);
            ok = ok && sequence.fragments().size() == expectedPieces;

            // Audio: un tratto silenziato a cavallo della prima giunzione (M), poi le forme d'onda pronte.
            const double muteFrom = std::max(0.0, sequence.startOf(1) - 0.1);
            window->setAudioSelection(muteFrom, muteFrom + 0.2);
            window->muteSelection();
            const auto muted = sequence.mutedRanges();
            QElapsedTimer waves;
            waves.start();
            while (window->pendingWaveforms() > 0 && waves.elapsed() < 20000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            // Annulla e Ripeti: il silenzio appena messo sparisce e torna; l'asterisco segue.
            {
                const QString label = window->undoText();
                const bool undone = window->undo() && sequence.mutedRanges().empty();
                const bool redone = window->redo() && sequence.mutedRanges().size() == 1;
                std::printf("Annulla / Ripeti: \"%s\" %s, ripetuto %s\n", qPrintable(label.mid(1)),
                            undone ? "annullato" : "NON annullato", redone ? "sì" : "NO");
                std::fflush(stdout);
                ok = ok && undone && redone && label.contains(QStringLiteral("Togli audio"));
            }

            // Miniature della timeline: si disegna la finestra (che le chiede) e si aspetta che siano pronte.
            {
                window->grab();
                QElapsedTimer thumbs;
                thumbs.start();
                while (window->pendingThumbnails() > 0 && thumbs.elapsed() < 20000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                const bool hasVideo = window->player()->statusText().contains(QStringLiteral("Decodifica"));
                std::printf("Miniature: %d pronte, %d in attesa\n", window->readyThumbnails(), window->pendingThumbnails());
                std::fflush(stdout);
                ok = ok && window->pendingThumbnails() == 0 && (!hasVideo || window->readyThumbnails() > 0);
            }

            // Zoom delle due linee del tempo: al massimo (12 pixel per fotogramma), disegnate, scorse fino in fondo e
            // di nuovo "Adatta". Con lo zoom al massimo si chiedono solo le miniature visibili.
            {
                bool zoomOk = true;
                for (ZoomableTimeline* timeline :
                     {static_cast<ZoomableTimeline*>(window->sequenceTimeline()), static_cast<ZoomableTimeline*>(window->player()->timeline())}) {
                    for (int i = 0; i < 80; ++i)
                        timeline->zoomIn();
                    const auto& zoom = timeline->zoom();
                    const bool atMax = zoom.duration() <= 0
                                    || std::abs(zoom.pixelsPerSecond() - zoom.maxPixelsPerSecond()) < 1e-6;
                    timeline->grab();
                    QWheelEvent wheel(QPointF(10, 10), timeline->mapToGlobal(QPointF(10, 10)), QPoint(), QPoint(0, -120 * 50),
                                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                    QCoreApplication::sendEvent(timeline, &wheel);
                    timeline->grab();
                    // Oltre "Adatta": tutto in un decimo della larghezza, senza barra di scorrimento.
                    for (int i = 0; i < 80; ++i)
                        timeline->zoomOut();
                    const bool atMin = zoom.duration() <= 0
                                    || (std::abs(zoom.contentWidth() - zoom.width() * 0.1) < 1.0 && !zoom.canScroll());
                    timeline->grab();
                    timeline->fitAll();
                    timeline->grab();
                    zoomOk = zoomOk && atMax && atMin && timeline->zoom().isFit();
                }
                QCoreApplication::processEvents();
                std::printf("Zoom delle linee del tempo: massimo, minimo e Adatta %s, miniature in attesa %d\n", zoomOk ? "OK" : "NON riusciti",
                            window->pendingThumbnails());
                std::fflush(stdout);
                ok = ok && zoomOk && window->pendingThumbnails() < 200;
                QElapsedTimer thumbs;
                thumbs.start();
                while (window->pendingThumbnails() > 0 && thumbs.elapsed() < 20000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }

            // Pannelli: la Sorgente staccata come finestra durante la riproduzione (il video deve continuare ad andare
            // e a mostrarsi), riagganciata, a schermo intero e ritorno; disposizione salvata e ripristinata.
            {
                const auto advances = [&](const char* when) {
                    const double from = player->currentPosition();
                    QElapsedTimer t;
                    t.start();
                    while (t.elapsed() < 700)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    const double moved = player->currentPosition() - from;
                    std::printf("  %s: la riproduzione avanza di %.2f s in 0,7 s (%s)\n", when, moved,
                                qPrintable(player->statusText()));
                    return moved > 0.3;
                };
                QDockWidget* source = window->sourceDock();
                player->seekTo(0.0);
                player->play();
                bool docksOk = advances("agganciata");
                source->setFloating(true);
                docksOk = advances("staccata") && source->isFloating() && docksOk;
                source->setFloating(false);
                docksOk = advances("riagganciata") && !source->isFloating() && docksOk;
                window->toggleFullScreen(source);
                const bool full = window->isPanelFullScreen() && source->isFloating();
                docksOk = advances("a schermo intero") && full && docksOk;
                window->toggleFullScreen(source);
                docksOk = docksOk && !window->isPanelFullScreen() && !source->isFloating();
                player->pause();

                // Disposizione: salvata, cambiata (pannello spostato e uno nascosto), ripristinata.
                const QByteArray state = window->saveState();
                const Qt::DockWidgetArea area = window->dockWidgetArea(window->filesDock());
                window->addDockWidget(Qt::RightDockWidgetArea, window->filesDock());
                window->timelineDock()->hide();
                const bool changed = window->dockWidgetArea(window->filesDock()) == Qt::RightDockWidgetArea;
                const bool restored = window->restoreState(state) && window->dockWidgetArea(window->filesDock()) == area
                                   && window->timelineDock()->isVisible();
                window->timelineDock()->hide();
                window->resetLayout();
                const bool reset = window->timelineDock()->isVisible() && !window->sourceDock()->isFloating();

                // La linea del tempo staccata e riagganciata torna sotto a tutto, larga quanto la finestra.
                QDockWidget* timelineDock = window->timelineDock();
                timelineDock->setFloating(true);
                QCoreApplication::processEvents();
                timelineDock->setFloating(false);
                window->addDockWidget(Qt::BottomDockWidgetArea, timelineDock);
                QCoreApplication::processEvents();
                const bool fullWidth = window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea
                                    && timelineDock->width() >= window->width() - 30;
                window->resetLayout();
                QCoreApplication::processEvents();

                // Incrocio fra il separatore verticale (file | Sorgente) e quello orizzontale (sopra | linea del tempo):
                // trascinandolo cambiano insieme la larghezza dei file e l'altezza della linea del tempo.
                QDockWidget* files = window->filesDock();
                const QRect fg = files->geometry();
                const QRect sg = window->sourceDock()->geometry();
                const QRect tg = timelineDock->geometry();
                const QPoint corner((fg.right() + sg.left()) / 2 + 1, (fg.bottom() + tg.top()) / 2 + 1);
                const int filesWidth = files->width();
                const int timelineHeight = timelineDock->height();
                const auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
                    QMouseEvent e(type, QPointF(at), window->mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &e);
                };
                mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseMove, corner + QPoint(40, -30), Qt::NoButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, corner + QPoint(40, -30), Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                const int widthChange = files->width() - filesWidth;
                const int heightChange = timelineDock->height() - timelineHeight;
                const bool cornerOk = std::abs(widthChange - 40) <= 4 && std::abs(heightChange - 30) <= 4;
                std::printf("  incrocio dei separatori in (%d, %d): file %+d px, linea del tempo %+d px (attesi +40 e +30)\n",
                            corner.x(), corner.y(), widthChange, heightChange);
                window->resetLayout();

                // Pannelli piccoli: la misura minima di ognuno e, con EDITORVIDEO_SMOKE_SCREENSHOT, un'immagine di ognuno
                // staccato a 170x120.
                bool smallOk = true;
                const QString shots = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
                for (QDockWidget* dock : {window->filesDock(), window->sourceDock(), window->programDock(), timelineDock}) {
                    const QSize minimum = dock->minimumSizeHint().expandedTo(dock->minimumSize());
                    std::printf("  pannello %s: misura minima %dx%d\n", qPrintable(dock->windowTitle()), minimum.width(),
                                minimum.height());
                    smallOk = smallOk && minimum.width() <= 170 && minimum.height() <= 140;
                    if (!shots.isEmpty()) {
                        dock->setFloating(true);
                        dock->resize(170, 120);
                        QCoreApplication::processEvents();
                        dock->grab().save(QString(shots).replace(QStringLiteral(".png"),
                                                                  QStringLiteral("_%1.png").arg(dock->objectName())));
                        dock->setFloating(false);
                    }
                }
                window->resetLayout();
                QCoreApplication::processEvents();
                std::printf("  linea del tempo riagganciata a tutta larghezza %s, incrocio %s, pannelli piccoli %s\n",
                            fullWidth ? "OK" : "NO", cornerOk ? "OK" : "NO", smallOk ? "OK" : "NO");
                docksOk = docksOk && fullWidth && cornerOk && smallOk;
                std::printf("Pannelli: staccati e riagganciati %s, disposizione salvata e ripristinata %s, ripristino iniziale %s\n",
                            docksOk ? "OK" : "NON riuscito", changed && restored ? "OK" : "NON riuscita",
                            reset ? "OK" : "NON riuscito");
                std::fflush(stdout);
                ok = ok && (docksOk || !player->hasMedia()) && changed && restored && reset;
            }

            // File trascinati da Esplora file (simulati con QMimeData): sulla linea del tempo del montaggio, in fondo, il file
            // (già nel progetto) va intero nella sequenza senza un doppione nell'elenco; poi una cartella sulla Sorgente.
            // Ogni trascinamento si annulla in un colpo solo.
            {
                SequenceTimeline* timeline = window->sequenceTimeline();
                const auto& seq = window->project().sequence();
                const QString file = QString::fromStdString(window->project().clips().front().path);
                const std::size_t pieces = seq.fragments().size();
                const std::size_t clips = window->project().clips().size();
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(file)});
                // Linea del tempo rimpicciolita oltre "Adatta": lo spazio vuoto a destra vale come "in fondo".
                for (int i = 0; i < 20; ++i)
                    timeline->zoomOut();
                const QPointF end(timeline->width() - 12, 30);
                QDragEnterEvent enter(end.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(timeline, &enter);
                QDropEvent drop(end, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(timeline, &drop);
                const bool inserted = enter.isAccepted() && drop.isAccepted() && seq.fragments().size() == pieces + 1
                                   && seq.fragments().back().clipId == seq.fragments().front().clipId
                                   && window->project().clips().size() == clips;
                timeline->fitAll();
                const QString label = window->undoText();
                const bool undone = window->undo() && seq.fragments().size() == pieces;

                // Una cartella sulla Sorgente (con una copia del file e un testo, che va ignorato): si aggiunge la
                // copia, che si apre nella Sorgente, e uno solo Annulla la toglie.
                QDir folder(QDir::temp().filePath(QStringLiteral("EditorVideoProvaTrascina")));
                folder.removeRecursively();
                QDir().mkpath(folder.path());
                QFile::copy(file, folder.filePath(QStringLiteral("copia.") + QFileInfo(file).suffix()));
                QFile note(folder.filePath(QStringLiteral("note.txt")));
                if (note.open(QIODevice::WriteOnly))
                    note.write("non è un video");
                note.close();
                window->dropFiles({QDir::toNativeSeparators(folder.path())}, MainWindow::DropTarget::Source);
                const std::size_t afterFolder = window->project().clips().size();
                const bool folderOk = afterFolder == clips + 1 && seq.fragments().size() == pieces
                                   && window->currentFile().contains(QStringLiteral("copia."));
                window->undo();
                const bool folderUndone = window->project().clips().size() == clips;
                QElapsedTimer settle;  // si lascia finire l'analisi della copia prima di cancellarla
                settle.start();
                while (settle.elapsed() < 3000 && !window->project().allAnalyzed())
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                std::printf("Trascinamento: nel montaggio %s (\"%s\" %s), cartella: %d file in più %s\n",
                            inserted ? "OK" : "NON riuscito", qPrintable(label.mid(1)), undone ? "annullato" : "NON annullato",
                            static_cast<int>(afterFolder - clips), folderOk && folderUndone ? "e annullati" : "NON riuscito");
                std::fflush(stdout);
                ok = ok && inserted && undone && label.contains(QStringLiteral("Aggiungi file")) && folderOk && folderUndone;
                folder.removeRecursively();
            }

            // Progetto: salvato e riaperto, con gli stessi pezzi e lo stesso audio tolto (e senza modifiche
            // in sospeso).
            {
                const QString projectFile =
                    QDir::temp().filePath(QStringLiteral("EditorVideoProvaProgetto") + core::kProjectExtension);
                const std::size_t pieces = sequence.fragments().size();
                const bool saved = window->saveProjectAs(projectFile, false) && !window->isModified();
                const bool reopened = saved && window->openProject(projectFile, false);
                const auto& again = window->project().sequence();
                const bool same = reopened && again.fragments().size() == pieces && again.mutedRanges().size() == 1
                               && !window->isModified();
                std::printf("Progetto: salvato %s, riaperto %s, %d pezzi\n", saved ? "sì" : "NO", same ? "uguale" : "DIVERSO",
                            static_cast<int>(again.fragments().size()));
                std::fflush(stdout);
                QFile::remove(projectFile);
                ok = ok && same;
                while (!window->project().allAnalyzed() && analysis.elapsed() < 40000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            std::printf("Audio: %d tratti senza audio (%.3f - %.3f s), forme d'onda %s\n", static_cast<int>(muted.size()),
                        muted.empty() ? 0.0 : muted.front().start, muted.empty() ? 0.0 : muted.front().end,
                        window->pendingWaveforms() == 0 ? "pronte" : "NON pronte");
            std::fflush(stdout);
            ok = ok && muted.size() == 1 && window->pendingWaveforms() == 0;

            // Anteprima del montaggio: da poco prima della prima giunzione, 1 s di riproduzione deve passare al
            // pezzo successivo e avanzare col tempo giusto.
            if (sequence.fragments().size() >= 2) {
                auto* program = window->programMonitor();
                const double join = sequence.startOf(1);
                const double from = std::max(0.0, join - 0.3);
                program->seekTo(from);
                program->togglePlayPause();
                QElapsedTimer playing;
                playing.start();
                while (playing.elapsed() < 1000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const double reached = program->position();
                const int piece = program->currentIndex();
                if (program->isPlaying())
                    program->togglePlayPause();
                const bool crossed = piece >= 1 && reached > join && reached < from + 1.6;
                std::printf("Anteprima del montaggio: da %.3f s a %.3f s, pezzo %d (giunzione a %.3f s)%s\n", from,
                            reached, piece + 1, join, crossed ? "" : "  <-- NON RIUSCITA");
                std::fflush(stdout);
                ok = ok && crossed;
            }
            const double expectedLength = window->exportLength();

            // Esportazione dei tratti come dal menu (senza chiedere dove salvare), con il messaggio finale.
            const QString target = QDir::temp().filePath(QStringLiteral("EditorVideoProvaEsportazione.")
                                                          + QFileInfo(window->currentFile()).suffix());
            QFile::remove(target);
            // Rallentata apposta, perché la finestra di avanzamento faccia in tempo a comparire, come con un file grande.
            if (!window->startExport(QDir::toNativeSeparators(target), true, std::chrono::milliseconds(25))) {
                std::printf("Esportazione: non partita\n");
                finish(false);
                return;
            }
            // Quando compare il messaggio finale si aspetta ancora un po' (più del ritardo con cui compare la
            // finestra di avanzamento) e si controlla che nessuna finestra di avanzamento sia rimasta aperta.
            auto* poll = new QTimer(&app);
            auto messageSeen = std::make_shared<QElapsedTimer>();
            auto waiting = std::make_shared<QElapsedTimer>();
            waiting->start();
            QObject::connect(poll, &QTimer::timeout, &app, [=] {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (box == nullptr) {
                    if (waiting->elapsed() < 15000)
                        return;
                    // Il messaggio finale non è mai arrivato in primo piano: di solito perché la finestra di
                    // avanzamento è rimasta aperta sopra.
                    poll->stop();
                    const QWidget* modal = QApplication::activeModalWidget();
                    std::printf("Esportazione: messaggio finale mai in primo piano (finestra attiva: %s, "
                                "esportazione %s, riuscita: %s)\n",
                                modal != nullptr ? qPrintable(modal->windowTitle()) : "nessuna",
                                window->isExporting() ? "ancora in corso" : "finita",
                                window->lastExportSucceeded() ? "sì" : qPrintable(QStringLiteral("no: ") + window->lastExportProblem()));
                    for (QWidget* w : QApplication::topLevelWidgets()) {
                        if (w->isVisible())
                            std::printf("  finestra visibile: \"%s\" (%s)\n", qPrintable(w->windowTitle()),
                                        w->metaObject()->className());
                    }
                    std::fflush(stdout);
                    if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                        dialog->reject();
                    finish(false);
                    return;
                }
                if (!messageSeen->isValid())
                    messageSeen->start();
                if (messageSeen->elapsed() < 700)
                    return;
                poll->stop();
                const auto dialogs = window->findChildren<QProgressDialog*>();
                const auto stuck = std::count_if(dialogs.begin(), dialogs.end(),
                                                 [](QProgressDialog* d) { return d->isVisible(); });
                bool exported = window->lastExportSucceeded() && QFileInfo::exists(target);
                if (exported) {
                    const double length = core::probeFile(target.toStdString()).durationSeconds;
                    std::printf("File esportato: %.3f s (attesi %.3f s)\n", length, expectedLength);
                    exported = std::abs(length - expectedLength) < 0.15;
                }
                std::printf("Esportazione: %s (\"%s\"), finestre di avanzamento rimaste aperte: %d\n",
                            exported ? "riuscita" : "NON riuscita", qPrintable(box->windowTitle()),
                            static_cast<int>(stuck));
                std::fflush(stdout);
                box->accept();
                QFile::remove(target);
                finish(ok && exported && stuck == 0);
            });
            poll->start(50);
        });
    }
}

}  // namespace editor::app
