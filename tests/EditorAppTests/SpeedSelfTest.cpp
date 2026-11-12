#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
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

void wait(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

}  // namespace

// --speed-test <video>: l'anteprima del Montaggio con pezzi a velocità diversa: il punto del file mostrato per un punto della
// sequenza, e la riproduzione che va a quella velocità.
int runSpeedTest(const QStringList& args, qsizetype at)
{
    SelfCheck check;
    std::printf("Prova dell'anteprima con la velocità dei pezzi\n");
    if (at + 1 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --speed-test <video>\n");
        return 1;
    }
    const std::string path = args[at + 1].toStdString();
    core::Project project;
    const core::MediaInfo info = core::probeFile(path);
    project.setFormat(core::formatOf(core::readMediaSignature(path)));
    const int clip = project.add(path);
    project.setAnalysis(clip, info.keyframeTimes, info.durationSeconds);
    const double length = std::min(info.durationSeconds, 6.0);
    const int piece = project.sequence().insert(-1, clip, 0.0, length, 2.0);  // dura la metà nel montaggio

    ProgramMonitor monitor(project);
    monitor.resize(900, 600);
    monitor.show();
    monitor.sequenceChanged();
    PlayerWidget* player = playerOf(monitor);
    check(player != nullptr, "il monitor ha il lettore");
    if (player == nullptr)
        return 3;
    const double frame = std::max(0.04, player->frameDuration());

    monitor.seekTo(1.0);
    QCoreApplication::processEvents();
    check(std::abs(monitor.position() - 1.0) < 0.05, "il cursore del montaggio è dove si è cercato");
    check(std::abs(player->currentPosition() - 2.0) <= 2 * frame, "a 2x il secondo 1 del montaggio è il secondo 2 del file");

    // Rallentatore: 0.5x, il secondo 1 del montaggio è il mezzo secondo del file.
    project.sequence().setSpeed(piece, 0.5, false, true);
    monitor.sequenceChanged();
    monitor.seekTo(1.0);
    QCoreApplication::processEvents();
    check(std::abs(player->currentPosition() - 0.5) <= 2 * frame, "a 0.5x il secondo 1 del montaggio è il mezzo secondo del file");

    // Riproduzione a 2x: in ~0.7 s di orologio il file avanza il doppio del montaggio.
    project.sequence().setSpeed(piece, 2.0, false, true);
    monitor.sequenceChanged();
    monitor.seekTo(0.0);
    QCoreApplication::processEvents();
    const double fileFrom = player->currentPosition();
    const double programFrom = monitor.position();
    monitor.setPlaying(true);
    wait(700);
    const double programDelta = monitor.position() - programFrom;
    const double fileDelta = player->currentPosition() - fileFrom;
    monitor.setPlaying(false);
    check(programDelta > 0.3 && programDelta < 1.2, "suonando il cursore del montaggio avanza");
    check(programDelta > 0.0 && fileDelta / programDelta > 1.6 && fileDelta / programDelta < 2.4,
          "il file avanza a velocità doppia rispetto al montaggio");

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
