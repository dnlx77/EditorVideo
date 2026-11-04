#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SelfTests.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/TransformManip.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace editor::app {

namespace core = editor::core;

// --manip-test <video>: i manipolatori sul monitor del Montaggio con un pezzo del file dato: maniglie visibili solo da
// fermo e con il tasto attivo, trascinare il corpo, un angolo e la maniglia di rotazione dà una sola modifica a gesto.
int runManipTest(const QStringList& args, qsizetype at)
{
    SelfCheck check;
    std::printf("Prova dei manipolatori sul monitor\n");
    if (at + 1 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --manip-test <video>\n");
        return 1;
    }
    const std::string path = args[at + 1].toStdString();
    core::Project project;
    const core::MediaInfo info = core::probeFile(path);
    project.setFormat(core::formatOf(core::readMediaSignature(path)));
    const int clip = project.add(path);
    project.setAnalysis(clip, info.keyframeTimes, info.durationSeconds);
    project.sequence().insert(-1, clip, 0.0, std::min(info.durationSeconds, 3.0));

    ProgramMonitor monitor(project);
    monitor.resize(900, 600);
    monitor.show();
    monitor.sequenceChanged();
    monitor.seekTo(0.3);
    QCoreApplication::processEvents();
    PlayerWidget* player = nullptr;
    for (QWidget* w : monitor.findChildren<QWidget*>()) {  // PlayerWidget non ha Q_OBJECT: findChild non lo accetta
        player = dynamic_cast<PlayerWidget*>(w);
        if (player != nullptr)
            break;
    }
    check(player != nullptr, "il monitor ha il lettore");
    if (player == nullptr)
        return 3;

    check(!monitor.manipulatorsVisible(), "a tasto spento le maniglie non si vedono");
    std::vector<std::pair<int, core::Transform>> edits;
    monitor.onTransformEdited = [&](int id, const core::Transform& t) { edits.emplace_back(id, t); };
    monitor.setTransformMode(true);
    QCoreApplication::processEvents();
    check(monitor.manipulatorsVisible(), "a tasto acceso e da fermo le maniglie si vedono");

    const QRectF canvas = player->imageRect();
    const QSize frame = player->currentFrameSize();
    check(!canvas.isEmpty() && !frame.isEmpty(), "il riquadro dell'immagine è noto");
    if (canvas.isEmpty() || frame.isEmpty())
        return 3;
    const double scale = canvas.width() / project.format().video()->width;  // pixel di schermo per pixel del progetto
    QWidget* view = player->childAt(canvas.center().toPoint());
    check(view != nullptr, "c'è un widget sotto l'immagine");
    if (view == nullptr)
        return 3;
    const auto send = [&](QEvent::Type type, QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent e(type, p, view->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
        QCoreApplication::sendEvent(view, &e);
    };
    const double projW = project.format().video()->width;
    const double projH = project.format().video()->height;
    const auto toScreen = [&](double x, double y) { return QPointF(canvas.left() + x * scale, canvas.top() + y * scale); };

    // Il corpo: dal centro a destra di 40 pixel del progetto.
    send(QEvent::MouseButtonPress, toScreen(projW / 2.0, projH / 2.0));
    send(QEvent::MouseMove, toScreen(projW / 2.0 + 20, projH / 2.0 + 10));
    check(edits.empty(), "durante il trascinamento non arriva ancora nessuna modifica");
    send(QEvent::MouseMove, toScreen(projW / 2.0 + 40, projH / 2.0 + 20));
    send(QEvent::MouseButtonRelease, toScreen(projW / 2.0 + 40, projH / 2.0 + 20));
    check(edits.size() == 1, "al rilascio arriva una sola modifica");
    if (edits.size() == 1)
        check(std::abs(edits[0].second.posX - 40) < 1.0 && std::abs(edits[0].second.posY - 20) < 1.0,
              "la posizione cambia di quanto trascinato");

    // Un angolo (basso-destra, da fermo e senza trasformazione): si ingrandisce.
    edits.clear();
    const QPointF corner = toScreen(projW, projH);
    send(QEvent::MouseButtonPress, corner);
    send(QEvent::MouseMove, toScreen(projW * 1.25, projH * 1.25));
    send(QEvent::MouseButtonRelease, toScreen(projW * 1.25, projH * 1.25));
    check(edits.size() == 1, "il gesto sull'angolo dà una sola modifica");
    if (edits.size() == 1)
        check(edits[0].second.scaleX > 1.1 && std::abs(edits[0].second.scaleX - edits[0].second.scaleY) < 1e-6,
              "l'angolo ingrandisce mantenendo le proporzioni");

    // La maniglia di rotazione, sopra il bordo alto: spostandola di lato il pezzo ruota.
    edits.clear();
    const QPointF handle = toScreen(projW / 2.0, 0.0) - QPointF(0, core::kManipRotateDistance);
    send(QEvent::MouseButtonPress, handle);
    send(QEvent::MouseMove, handle + QPointF(projW * scale / 4, core::kManipRotateDistance));
    send(QEvent::MouseButtonRelease, handle + QPointF(projW * scale / 4, core::kManipRotateDistance));
    check(edits.size() == 1 && std::abs(edits[0].second.rotation) > 5.0, "la maniglia di rotazione ruota il pezzo");

    // Un clic fermo sul corpo non cambia niente.
    edits.clear();
    send(QEvent::MouseButtonPress, canvas.center());
    send(QEvent::MouseButtonRelease, canvas.center());
    check(edits.empty(), "un clic fermo non cambia nulla");

    monitor.setPlaying(true);
    check(!monitor.manipulatorsVisible(), "in riproduzione le maniglie spariscono");
    monitor.setPlaying(false);
    monitor.setTransformMode(false);
    check(!monitor.manipulatorsVisible(), "a tasto spento spariscono");

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
