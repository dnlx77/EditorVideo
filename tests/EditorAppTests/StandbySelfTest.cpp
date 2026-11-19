#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/TestAccess.h"
#include "SelfTests.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace editor::app {

namespace core = editor::core;

namespace {

PlayerWidget* playerOf(ProgramMonitor& monitor)
{
    for (QWidget* w : monitor.findChildren<QWidget*>()) {  // PlayerWidget non ha Q_OBJECT: findChild non lo accetta
        if (auto* player = dynamic_cast<PlayerWidget*>(w))
            return player;
    }
    return nullptr;
}

}  // namespace

// --standby-test <video>: nell'anteprima del Montaggio, a pochi secondi dal pezzo dopo il lettore ne prepara già il file (in un
// thread a parte), e al cambio lo usa invece di riaprirlo: niente pausa fra un pezzo e l'altro.
int runStandbyTest(const QStringList& args, qsizetype at)
{
    SelfCheck check;
    std::printf("Prova della preparazione del pezzo dopo nell'anteprima\n");
    if (at + 1 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --standby-test <video>\n");
        return 1;
    }
    const std::string path = args[at + 1].toStdString();
    core::Project project;
    const core::MediaInfo info = core::probeFile(path);
    project.setFormat(core::formatOf(core::readMediaSignature(path)));
    const double length = std::min(info.durationSeconds, 3.0);
    // Due «file» (stesso video, due voci del progetto): al cambio di pezzo il lettore deve aprire l'altro.
    int clips[2];
    for (int& clip : clips) {
        clip = project.add(path);
        project.setAnalysis(clip, info.keyframeTimes, info.durationSeconds);
        project.sequence().insert(-1, clip, 0.0, length, 1.0);
    }

    ProgramMonitor monitor(project);
    monitor.resize(900, 600);
    monitor.show();
    monitor.sequenceChanged();
    PlayerWidget* player = playerOf(monitor);
    check(player != nullptr, "il monitor ha il lettore");
    if (player == nullptr)
        return 3;
    check(TestAccess::standbyAdopted(*player) == 0, "all'inizio nessuna preparazione usata");

    monitor.seekTo(std::max(0.0, length - 1.0));
    monitor.setPlaying(true);
    QElapsedTimer timer;
    timer.start();
    while (monitor.currentIndex() != 1 && timer.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    check(monitor.currentIndex() == 1, "la riproduzione passa al secondo pezzo");
    check(TestAccess::standbyAdopted(*player) == 1, "il secondo pezzo è stato aperto da quello preparato in anticipo");
    // Il secondo pezzo parte dal suo inizio (non da 0 del file per caso: qui coincidono, ma il tempo avanza da lì).
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    check(monitor.position() >= length - 0.05, "il tempo del montaggio continua senza tornare indietro");
    monitor.setPlaying(false);

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
