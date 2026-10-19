#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kSmokePlayMilliseconds = 2000;

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

    editor::app::MainWindow window;
    window.show();

    // "EditorVideo.exe file.mp4" (o un file trascinato sull'icona) apre subito quel file.
    const QStringList args = QApplication::arguments();
    QString file;
    for (qsizetype i = 1; i < args.size(); ++i) {
        if (!args[i].startsWith(QStringLiteral("--"))) {
            file = args[i];
            window.openPath(file);
            break;
        }
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

                // Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png salva anche un'immagine della finestra.
                const QString screenshot = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
                if (!screenshot.isEmpty()) {
                    QCoreApplication::processEvents();  // applica i cambi di layout ancora in sospeso
                    window->grab().save(screenshot);
                }
                app.exit(ok ? 0 : 2);
            });
        }
    }

    return app.exec();
}
