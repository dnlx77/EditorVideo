#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentList.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/SegmentExport.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

constexpr int kSmokePlayMilliseconds = 2000;

// --export-test <origine> <destinazione> <IN s> <OUT s> [<IN s> <OUT s> ...]: esporta uno o più tratti come farebbe
// l'interfaccia (ogni IN agganciato al fotogramma chiave, tratti uniti nell'ordine del tempo) e controlla il file
// creato. Codice di uscita 0 se è tutto giusto.
// --join-test <destinazione> <file1> <file2> ...: unisce i file interi come farebbe l'interfaccia (dopo il controllo
// di compatibilità) e controlla il file creato. Codice di uscita 0 se è tutto giusto, 4 se i file non si possono
// unire (il motivo viene scritto), 2 per altri errori, 3 se il controllo del risultato non riesce.
int runJoinTest(const QStringList& args, qsizetype at)
{
    namespace core = editor::core;
    if (at + 3 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --join-test <destinazione> <file1> <file2> [...]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    try {
        core::Project project;
        std::vector<core::MediaSignature> signatures;
        std::vector<core::MediaInfo> sources;
        for (qsizetype i = at + 2; i < args.size(); ++i) {
            const std::string path = args[i].toStdString();
            sources.push_back(core::probeFile(path));
            signatures.push_back(core::readMediaSignature(path));
            project.setAnalysis(project.add(path), sources.back().keyframeTimes, sources.back().durationSeconds);
        }
        if (const auto mismatches = core::checkCompatibility(signatures); !mismatches.empty()) {
            std::printf("Non si possono unire:\n%s\n", core::describeMismatches(mismatches).c_str());
            return 4;
        }
        const core::ExportResult result = core::exportClips(project.exportPlan(), output);
        const core::MediaInfo joined = core::probeFile(output);
        std::printf("Uniti %d file: durata attesa %.3f s, ottenuta %.3f s, %d flussi\n", result.clipCount,
                    project.totalLength(), joined.durationSeconds, result.streamCount);

        const double tolerance = 0.1 + 0.05 * static_cast<double>(result.clipCount);
        bool ok = std::abs(joined.durationSeconds - project.totalLength()) < tolerance
               && joined.streams.size() == static_cast<std::size_t>(result.streamCount);
        for (std::size_t s = 0; ok && s < joined.streams.size() && s < sources.front().streams.size(); ++s)
            ok = joined.streams[s].codec == sources.front().streams[s].codec;
        if (joined.keyframeStreamIndex >= 0)
            ok = ok && !joined.keyframeTimes.empty() && std::abs(joined.keyframeTimes.front()) < 0.001;
        std::printf("%s\n", ok ? "Controllo del file unito: OK" : "Controllo del file unito: NON RIUSCITO");
        return ok ? 0 : 3;
    } catch (const std::exception& e) {
        std::printf("Errore: %s\n", e.what());
        return 2;
    }
}

int runExportTest(const QStringList& args, qsizetype at)
{
    namespace core = editor::core;
    const qsizetype pairs = (args.size() - at - 3) / 2;
    if (at + 4 >= args.size() || (args.size() - at - 3) % 2 != 0) {
        std::printf("Uso: EditorVideo.exe --export-test <origine> <destinazione> <IN s> <OUT s> [<IN s> <OUT s> ...]\n");
        return 1;
    }
    const std::string input = args[at + 1].toStdString();
    const std::string output = args[at + 2].toStdString();
    try {
        const core::MediaInfo source = core::probeFile(input);
        core::SegmentList list;
        list.reset(source.keyframeTimes, source.durationSeconds);
        for (qsizetype i = 0; i < pairs; ++i) {
            const double in = args[at + 3 + 2 * i].toDouble();
            const double out = args[at + 4 + 2 * i].toDouble();
            if (!list.add(in, out))
                throw core::MediaError("OUT deve stare dopo IN.");
        }
        for (const core::TimeRange& r : list.ranges())
            std::printf("Tratto: %.3f - %.3f s\n", r.start, r.end);

        const core::ExportResult result = core::exportSegments(input, output, list.ranges());
        const core::MediaInfo exported = core::probeFile(output);
        std::printf("Esportato: %d tratti, durata attesa %.3f s, ottenuta %.3f s, %d flussi\n", result.segmentCount,
                    list.totalLength(), exported.durationSeconds, result.streamCount);

        // Tolleranza: un paio di fotogrammi per giunzione.
        const double tolerance = 0.1 + 0.05 * static_cast<double>(list.ranges().size() - 1);
        bool ok = exported.streams.size() == static_cast<std::size_t>(result.streamCount)
               && std::abs(exported.durationSeconds - list.totalLength()) < tolerance;
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
    } catch (const std::exception& e) {
        std::printf("Errore: %s\n", e.what());
        return 2;
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    // --smoke-test: apre la finestra, scrive le versioni su stdout e chiude subito.
    // Serve alla build automatica per verificare che l'eseguibile parta con tutte le DLL e i plugin Qt.
    // Con anche un file ("--smoke-test video.mkv") lo riproduce per 2 secondi, scrive a che punto è arrivato
    // e prova a segnare IN e OUT.
    bool smokeTest = false;
    for (int i = 1; i < argc; ++i)
        smokeTest = smokeTest || std::strcmp(argv[i], "--smoke-test") == 0;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("EditorVideo"));

    // I percorsi si leggono da QApplication::arguments(): argv su Windows perde le lettere accentate.
    const QStringList arguments = QApplication::arguments();
    if (const qsizetype at = arguments.indexOf(QStringLiteral("--export-test")); at >= 0) {
        const int code = runExportTest(arguments, at);
        std::fflush(stdout);
        return code;
    }
    if (const qsizetype at = arguments.indexOf(QStringLiteral("--join-test")); at >= 0) {
        const int code = runJoinTest(arguments, at);
        std::fflush(stdout);
        return code;
    }

    editor::app::MainWindow window;
    window.show();

    // "EditorVideo.exe file.mp4" (o un file trascinato sull'icona) apre subito quel file; altri file dopo il primo
    // vengono aggiunti al progetto, da unire.
    QStringList files;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        if (!arguments[i].startsWith(QStringLiteral("--")))
            files.append(arguments[i]);
    }
    const QString file = files.isEmpty() ? QString() : files.front();
    if (!files.isEmpty()) {
        window.openPath(files.front());
        if (files.size() > 1)
            window.addFiles(files.mid(1));
    }

    if (smokeTest) {
        std::printf("Piattaforma Qt: %s\n%s\n", qPrintable(QGuiApplication::platformName()),
                    qPrintable(editor::app::versionSummary()));
        std::fflush(stdout);
        if (file.isEmpty()) {
            QTimer::singleShot(0, &app, &QApplication::quit);
        } else {
            editor::app::PlayerWidget* player = window.player();
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
                const editor::core::CutSegment& segment = player->segment();
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

                // Due tratti nell'elenco: quello appena segnato e uno all'inizio del file.
                player->addRange();
                player->seekTo(0.0);
                player->markIn();
                for (int i = 0; i < 3; ++i)
                    player->stepFrame(+1);
                player->markOut();
                player->addRange();
                const editor::core::SegmentList& ranges = player->ranges();
                std::printf("Tratti: %d, durata finale %.3f s\n", static_cast<int>(ranges.ranges().size()),
                            ranges.totalLength());
                std::fflush(stdout);
                ok = ok && ranges.ranges().size() == 2;

                // Con più file si aspetta che siano stati analizzati tutti (servono durata e fotogrammi chiave).
                QElapsedTimer analysis;
                analysis.start();
                while (!window->project().allAnalyzed() && analysis.elapsed() < 20000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                const double expectedLength = window->exportLength();
                if (window->project().clips().size() > 1) {
                    std::printf("Progetto: %d file, durata finale %.3f s\n",
                                static_cast<int>(window->project().clips().size()), expectedLength);
                    std::fflush(stdout);
                }

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
                        const double length = editor::core::probeFile(target.toStdString()).durationSeconds;
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

    return app.exec();
}
