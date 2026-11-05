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

    // Gli scatti magnetici: uno spostamento di pochi pixel dal centro torna sul centro (niente modifica), con Ctrl no.
    edits.clear();
    send(QEvent::MouseButtonPress, canvas.center());
    send(QEvent::MouseMove, canvas.center() + QPointF(3, 2));
    send(QEvent::MouseButtonRelease, canvas.center() + QPointF(3, 2));
    // (la forma del fotogramma mostrato può differire di un soffio da quella del progetto: si tollera mezzo pixel)
    check(edits.empty() || (edits.size() == 1 && std::abs(edits[0].second.posX) < 0.5 && std::abs(edits[0].second.posY) < 0.5),
          "un piccolo spostamento scatta sul centro del fotogramma");
    edits.clear();
    send(QEvent::MouseButtonPress, handle);
    send(QEvent::MouseMove, handle + QPointF(4, 0));
    send(QEvent::MouseButtonRelease, handle + QPointF(4, 0));
    check(edits.empty(), "una rotazione di un grado scatta a zero");

    // Ritaglia (W): si esclude con Trasforma; un lato e un angolo.
    monitor.setCropMode(true);
    QCoreApplication::processEvents();
    check(monitor.cropMode() && !monitor.transformMode() && monitor.manipulatorsVisible(),
          "il ritaglio spegne la modalità Trasforma e mostra le maniglie");
    edits.clear();
    send(QEvent::MouseButtonPress, toScreen(0, projH / 2.0));
    send(QEvent::MouseMove, toScreen(projW * 0.1, projH / 2.0));
    send(QEvent::MouseMove, toScreen(projW * 0.25, projH / 2.0));
    send(QEvent::MouseButtonRelease, toScreen(projW * 0.25, projH / 2.0));
    check(edits.size() == 1, "il gesto sul lato sinistro dà una sola modifica");
    if (edits.size() == 1)
        check(std::abs(edits[0].second.cropLeft - 0.25) < 0.01 && edits[0].second.cropRight == 0.0 && edits[0].second.cropTop == 0.0,
              "il lato sinistro ritaglia un quarto e nient'altro");
    edits.clear();
    send(QEvent::MouseButtonPress, toScreen(projW, 0));
    send(QEvent::MouseMove, toScreen(projW * 0.9, projH * 0.2));
    send(QEvent::MouseButtonRelease, toScreen(projW * 0.9, projH * 0.2));
    check(edits.size() == 1, "il gesto sull'angolo dà una sola modifica");
    if (edits.size() == 1)
        check(std::abs(edits[0].second.cropRight - 0.1) < 0.01 && std::abs(edits[0].second.cropTop - 0.2) < 0.01,
              "l'angolo alto a destra ritaglia due lati");
    monitor.setTransformMode(true);
    check(monitor.transformMode() && !monitor.cropMode(), "Trasforma spegne il ritaglio");
    monitor.setTransformMode(false);
    monitor.setThirdsGrid(true);
    check(monitor.thirdsGrid(), "la griglia dei terzi si accende");
    monitor.setThirdsGrid(false);
    monitor.setTransformMode(true);

    // Fotogrammi chiave (K2): il monitor disegna il pezzo con la trasformazione dell'istante, a ogni posizione.
    monitor.setTransformMode(false);
    {
        const core::Fragment& fragment = project.sequence().fragments()[0];
        core::TransformAnimation animation;
        animation.track(core::AnimParam::ScaleX) = {{.time = 0.5, .value = 0.5}, {.time = 1.5, .value = 1.0}};
        animation.track(core::AnimParam::ScaleY) = animation.track(core::AnimParam::ScaleX);
        animation.track(core::AnimParam::Opacity) = {{.time = 0.0, .value = 0.2}};
        project.sequence().setAnimation(fragment.id, animation);
        monitor.sequenceChanged();
        monitor.seekTo(0.5);
        QCoreApplication::processEvents();
        check(std::abs(player->drawnTransform().scaleX - 0.5) < 1e-6 && std::abs(player->drawnTransform().opacity - 0.2) < 1e-6,
              "al primo fotogramma chiave il pezzo è disegnato con il suo valore");
        monitor.seekTo(1.0);
        QCoreApplication::processEvents();
        check(std::abs(player->drawnTransform().scaleX - 0.75) < 0.03,
              "a metà fra due fotogrammi chiave la scala è interpolata (75%)");
        monitor.seekTo(2.0);
        QCoreApplication::processEvents();
        check(std::abs(player->drawnTransform().scaleX - 1.0) < 1e-6, "dopo l'ultimo fotogramma chiave resta il suo valore");
        project.sequence().setAnimation(fragment.id, {});
        monitor.sequenceChanged();
        monitor.seekTo(1.0);
        QCoreApplication::processEvents();
        check(player->drawnTransform().isIdentity(), "senza fotogrammi chiave il pezzo torna senza trasformazione");
    }

    monitor.setTransformMode(true);
    monitor.setPlaying(true);
    check(!monitor.manipulatorsVisible(), "in riproduzione le maniglie spariscono");
    monitor.setPlaying(false);
    monitor.setTransformMode(false);
    check(!monitor.manipulatorsVisible(), "a tasto spento spariscono");

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
