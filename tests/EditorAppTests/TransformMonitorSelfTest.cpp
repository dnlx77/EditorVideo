#include "EditorApp/CurveEditorDialog.h"
#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/PropertiesPanel.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorApp/SequenceTimeline.h"
#include "SelfTests.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/TransformManip.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QElapsedTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace editor::app {

namespace core = editor::core;

namespace {
double evaluateTrackForTest(const std::vector<core::AnimKey>& keys, double time)
{
    return core::evaluateTrack(keys, time, 0.0);
}
}  // namespace

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

    // Percorso del movimento (K6): con la posizione animata il monitor mostra un punto per chiave, e un punto si trascina.
    {
        const core::Fragment& fragment = project.sequence().fragments()[0];
        core::TransformAnimation animation;
        animation.track(core::AnimParam::PosX) = {{.time = 0.5, .value = 0.0}, {.time = 2.5, .value = 200.0}};
        animation.track(core::AnimParam::PosY) = {{.time = 0.5, .value = 0.0}, {.time = 2.5, .value = 100.0}};
        project.sequence().setAnimation(fragment.id, animation);
        monitor.sequenceChanged();
        monitor.seekTo(1.0);
        monitor.setTransformMode(true);
        QCoreApplication::processEvents();
        const auto path = monitor.motionPath();
        check(path.has_value() && path->dots.size() == 2 && path->line.size() > 10, "con la posizione animata il monitor ha il percorso del movimento");
        if (path.has_value() && path->dots.size() == 2) {
            check(std::abs(path->dots[0].pos.x - projW / 2.0) < 1e-6 && std::abs(path->dots[1].pos.x - (projW / 2.0 + 200.0)) < 1e-6
                      && std::abs(path->dots[1].pos.y - (projH / 2.0 + 100.0)) < 1e-6 && std::abs(path->dots[1].fileTime - 2.5) < 1e-9,
                  "i punti del percorso stanno dove il pezzo è alle chiavi");
            std::vector<std::array<double, 3>> moved;
            monitor.onMotionKeyMoved = [&](bool, int, double t, double x, double y) { moved.push_back({t, x, y}); };
            edits.clear();
            const core::Point2 dot = path->dots[1].pos;
            send(QEvent::MouseButtonPress, toScreen(dot.x, dot.y));
            send(QEvent::MouseMove, toScreen(dot.x + 25, dot.y + 10));
            send(QEvent::MouseMove, toScreen(dot.x + 50, dot.y + 20));
            check(moved.empty(), "durante il trascinamento di un punto non arriva ancora nessuna modifica");
            send(QEvent::MouseButtonRelease, toScreen(dot.x + 50, dot.y + 20));
            check(moved.size() == 1 && std::abs(moved[0][0] - 2.5) < 1e-9 && std::abs(moved[0][1] - 250.0) < 1.0
                      && std::abs(moved[0][2] - 120.0) < 1.0,
                  "al rilascio arrivano l'istante della chiave e la nuova posizione");
            check(edits.empty(), "trascinare un punto del percorso non sposta il pezzo");
            monitor.onMotionKeyMoved = nullptr;  // `moved` finisce qui: il monitor non deve più scriverci
        }
        // Una chiave sola: non c'è un percorso.
        animation.track(core::AnimParam::PosX) = {{.time = 0.5, .value = 10.0}};
        animation.track(core::AnimParam::PosY).clear();
        project.sequence().setAnimation(fragment.id, animation);
        monitor.sequenceChanged();
        check(!monitor.motionPath().has_value(), "con una chiave sola non c'è il percorso");
        project.sequence().setAnimation(fragment.id, {});
        monitor.sequenceChanged();
        monitor.setTransformMode(false);
    }

    // --- Le maniglie su una scritta (S4): scala 100% = la grandezza della scritta; un gesto = una modifica.
    {
        core::TextClip tc;
        tc.text = "Prova";
        tc.position = 0.0;
        tc.length = 3.0;
        const int textId = project.textTracks().add(project.textTracks().addLayer(), tc);
        check(textId != 0, "una scritta per provare le maniglie");
        monitor.selectedText = [&] { return textId; };
        std::vector<core::Transform> textEdits;
        monitor.onTextTransformEdited = [&](int, const core::Transform& t) { textEdits.push_back(t); };
        monitor.sequenceChanged();
        monitor.seekTo(1.0);
        monitor.setTransformMode(true);
        QCoreApplication::processEvents();
        const auto quad = monitor.manipQuad();
        check(monitor.manipulatorsVisible() && quad.has_value(), "con la scritta scelta le maniglie sono sulla scritta");
        if (quad.has_value()) {
            const double w = (*quad)[1].x - (*quad)[0].x;
            const double h = (*quad)[3].y - (*quad)[0].y;
            check(w > 10 && h > 5 && w < projW && h < projH / 2, "il riquadro ha la grandezza della scritta, non quella del fotogramma");
            const core::Point2 c{((*quad)[0].x + (*quad)[2].x) / 2, ((*quad)[0].y + (*quad)[2].y) / 2};
            // Spostamento: dal centro del riquadro, di 60 e 30 pixel (Ctrl non c'è: i bordi del fotogramma sono lontani).
            send(QEvent::MouseButtonPress, toScreen(c.x, c.y));
            send(QEvent::MouseMove, toScreen(c.x + 30, c.y + 15));
            send(QEvent::MouseMove, toScreen(c.x + 60, c.y + 30));
            check(textEdits.empty(), "durante il gesto sulla scritta non arriva nessuna modifica");
            send(QEvent::MouseButtonRelease, toScreen(c.x + 60, c.y + 30));
            check(textEdits.size() == 1 && std::abs(textEdits[0].posX - 60.0) < 6.0 && std::abs(textEdits[0].posY - 30.0) < 6.0,
                  "spostare il riquadro dà una sola modifica con lo spostamento della scritta");
            // Scala: l'angolo in basso a destra, portato al doppio (l'angolo in alto a sinistra resta fermo): scala 2 e centro spostato.
            textEdits.clear();
            const core::Point2 corner = (*quad)[2];
            const core::Point2 origin = (*quad)[0];
            const core::Point2 target{origin.x + 2 * (corner.x - origin.x), origin.y + 2 * (corner.y - origin.y)};
            send(QEvent::MouseButtonPress, toScreen(corner.x, corner.y));
            send(QEvent::MouseMove, toScreen(target.x, target.y));
            send(QEvent::MouseButtonRelease, toScreen(target.x, target.y));
            check(textEdits.size() == 1 && std::abs(textEdits[0].scaleX - 2.0) < 0.15 && std::abs(textEdits[0].scaleY - 2.0) < 0.15,
                  "la scala della scritta è in unità vere (2 = il doppio della scritta)");
            check(textEdits.size() == 1 && std::abs(textEdits[0].posX - w / 2.0) < 8.0 && std::abs(textEdits[0].posY - h / 2.0) < 8.0,
                  "con l'angolo opposto fermo il centro della scritta si sposta di mezza scritta");
        }
        monitor.selectedText = nullptr;
        monitor.setTransformMode(false);
    }

    monitor.setTransformMode(true);
    monitor.setPlaying(true);
    check(!monitor.manipulatorsVisible(), "in riproduzione le maniglie spariscono");
    monitor.setPlaying(false);
    monitor.setTransformMode(false);
    check(!monitor.manipulatorsVisible(), "a tasto spento spariscono");

    // --- Fotogrammi chiave dal pannello Proprietà (K4), con la finestra vera.
    {
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        window.openPath(QString::fromStdString(path));
        QElapsedTimer waiting;
        waiting.start();
        while (!window.project().allAnalyzed() && waiting.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        window.addToSequence();
        QCoreApplication::processEvents();
        const auto& fragments = window.project().sequence().fragments();
        check(fragments.size() == 1, "la finestra ha un pezzo nella sequenza");
        if (fragments.size() == 1) {
            const int id = fragments[0].id;
            window.sequenceTimeline()->selectObjects({{core::ObjectRef::Kind::Piece, id}});
            QCoreApplication::processEvents();
            PropertiesPanel* panel = window.propertiesPanel();
            ProgramMonitor* program = window.programMonitor();
            const auto animation = [&]() -> const core::TransformAnimation& { return window.project().sequence().find(id)->animation; };
            const auto seek = [&](double seconds) {
                program->seekTo(seconds);
                QElapsedTimer settle;  // il fotogramma arriva da un altro thread: si aspetta che il cursore sia lì
                settle.start();
                while (std::abs(program->position() - seconds) > 0.05 && settle.elapsed() < 3000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QCoreApplication::processEvents();
            };
            auto* key = panel->findChild<QToolButton*>(QStringLiteral("chiave1"));  // gruppo Posizione
            auto* next = panel->findChild<QToolButton*>(QStringLiteral("chiaveDopo1"));
            auto* prev = panel->findChild<QToolButton*>(QStringLiteral("chiavePrima1"));
            QDoubleSpinBox* posX = nullptr;  // (si prende quello visibile)
            for (QDoubleSpinBox* spin : panel->findChildren<QDoubleSpinBox*>(QStringLiteral("posizioneX"))) {
                if (spin->isVisibleTo(panel))
                    posX = spin;
            }
            check(panel->showingTransform() && key != nullptr && next != nullptr && prev != nullptr && posX != nullptr,
                  "il pannello mostra le trasformazioni con il rombo della posizione");
            if (panel->showingTransform() && key != nullptr && next != nullptr && prev != nullptr && posX != nullptr) {
                seek(0.5);
                check(key->isEnabled() && key->text() == QStringLiteral("◇") && !next->isEnabled() && !prev->isEnabled(),
                      "col cursore nel pezzo il rombo è attivo e vuoto, senza chiavi vicine");
                key->click();
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::PosX).size() == 1 && animation().track(core::AnimParam::PosY).size() == 1
                          && !animation().animated(core::AnimParam::ScaleX),
                      "il rombo mette una chiave per X e Y della posizione, e nient'altro");
                check(key->text() == QStringLiteral("◆"), "sul cursore il rombo è pieno");

                // Auto-key: cambiare il valore di un parametro animato, altrove, mette una chiave lì.
                seek(1.5);
                check(key->text() == QStringLiteral("◇") && prev->isEnabled(), "più avanti il rombo è vuoto e la freccia indietro è attiva");
                posX->setValue(100.0);
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::PosX).size() == 2
                          && std::abs(animation().track(core::AnimParam::PosX)[1].value - 100.0) < 1e-6,
                      "cambiare la posizione con l'animazione accesa mette una chiave sul cursore");
                check(std::abs(window.project().sequence().find(id)->transform.posX) < 1e-9,
                      "la trasformazione fissa del pezzo non cambia");

                // Fra le due chiavi il pannello mostra il valore interpolato.
                const double first = animation().track(core::AnimParam::PosX)[0].time;
                const double second = animation().track(core::AnimParam::PosX)[1].time;
                seek((first + second) / 2.0);
                const double expected = evaluateTrackForTest(animation().track(core::AnimParam::PosX), program->position());
                check(std::abs(posX->value() - expected) < 6.0 && posX->value() > 20.0 && posX->value() < 80.0,
                      "a metà fra le chiavi il pannello mostra un valore intermedio");

                // Le frecce vanno alle chiavi.
                seek(first);
                next->click();
                QCoreApplication::processEvents();
                check(std::abs(program->position() - second) < 0.05, "la freccia avanti va alla chiave dopo");
                prev->click();
                QCoreApplication::processEvents();
                check(std::abs(program->position() - first) < 0.05, "la freccia indietro va alla chiave prima");

                // Il rombo su una chiave la toglie.
                seek(second);
                check(key->text() == QStringLiteral("◆"), "sulla seconda chiave il rombo è pieno");
                key->click();
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::PosX).size() == 1, "il rombo su una chiave la toglie");

                // Il ripristino del gruppo spegne l'animazione.
                std::vector<QToolButton*> resets;
                for (QToolButton* b : panel->findChildren<QToolButton*>()) {
                    if (b->text() == QStringLiteral("↺"))
                        resets.push_back(b);
                }
                check(resets.size() >= 2, "ci sono i pulsanti di ripristino");
                if (resets.size() >= 2) {
                    resets[1]->click();  // il secondo gruppo: la posizione
                    QCoreApplication::processEvents();
                    check(!animation().animated(core::AnimParam::PosX) && !animation().animated(core::AnimParam::PosY),
                          "ripristinare la posizione spegne la sua animazione");
                }
            }

            // --- La riga dei rombi sulla linea del tempo (K5).
            {
                SequenceTimeline* line = window.sequenceTimeline();
                const core::Fragment& piece = *window.project().sequence().find(id);
                const double t1 = piece.start + 0.5;
                const double t2 = piece.start + 1.5;
                window.editAnimationKeys(QStringLiteral("prova"), [&](core::TransformAnimation& a, core::Transform&) {
                    core::AnimKey k;
                    k.time = t1;
                    k.value = 1.0;
                    core::setKey(a, core::AnimParam::Opacity, k);
                    k.time = t2;
                    k.value = 0.2;
                    core::setKey(a, core::AnimParam::Opacity, k);
                    return true;
                });
                QCoreApplication::processEvents();
                check(line->keyRow().keys.size() == 2 && !line->keyRow().layerPiece && line->keyRow().id == id,
                      "la riga dei rombi mostra le due chiavi del pezzo scelto");
                const auto send = [&](QEvent::Type type, const QPointF& p, bool left) {
                    QMouseEvent e(type, p, line->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                  left ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(line, &e);
                };
                const QPointF first = line->keyCenterForTest(0);
                check(first.x() > 0 && first.y() > 0, "il primo rombo si vede sulla linea del tempo");
                // Un clic sceglie il rombo; Canc non toglie il pezzo ma la chiave.
                send(QEvent::MouseButtonPress, first, true);
                send(QEvent::MouseButtonRelease, first, false);
                check(std::abs(line->selectedKey() - t1) < 1e-6, "il clic sul rombo sceglie la chiave");
                check(window.project().sequence().fragments().size() == 1, "il clic sul rombo non cambia il pezzo");
                // Trascinare il rombo sposta la chiave (fra l'altra e la fine del pezzo).
                const QPointF second = line->keyCenterForTest(1);
                const QPointF target(first.x() + (second.x() - first.x()) * 0.5, first.y());
                send(QEvent::MouseButtonPress, first, true);
                send(QEvent::MouseMove, QPointF(first.x() + 8, first.y()), true);
                send(QEvent::MouseMove, target, true);
                send(QEvent::MouseButtonRelease, target, false);
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::Opacity).size() == 2, "spostare un rombo non cambia il numero di chiavi");
                if (animation().track(core::AnimParam::Opacity).size() == 2) {
                    const double moved = animation().track(core::AnimParam::Opacity)[0].time;
                    check(moved > t1 + 0.2 && moved < t2 - 0.2, "la chiave si è spostata dove si è lasciato il rombo");
                    check(std::abs(animation().track(core::AnimParam::Opacity)[0].value - 1.0) < 1e-9, "il valore della chiave resta");
                }
                // Trascinarlo sull'altro non è permesso: la chiave resta dov'era.
                const QPointF a = line->keyCenterForTest(0);
                const QPointF b = line->keyCenterForTest(1);
                const double before = animation().track(core::AnimParam::Opacity)[0].time;
                send(QEvent::MouseButtonPress, a, true);
                send(QEvent::MouseMove, QPointF(a.x() + 10, a.y()), true);
                send(QEvent::MouseMove, b, true);
                send(QEvent::MouseButtonRelease, b, false);
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::Opacity).size() == 2
                          && std::abs(animation().track(core::AnimParam::Opacity)[0].time - before) < 1e-6,
                      "una chiave non si lascia sopra un'altra");
                // Doppio clic: il cursore va lì.
                {
                    const QPointF p = line->keyCenterForTest(1);
                    QMouseEvent e(QEvent::MouseButtonDblClick, p, line->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(line, &e);
                    QElapsedTimer settle;
                    settle.start();
                    while (std::abs(program->position() - 1.5) > 0.05 && settle.elapsed() < 3000)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    check(std::abs(program->position() - 1.5) < 0.1, "il doppio clic su un rombo porta il cursore sulla chiave");
                }
                // La mini linea del tempo del pannello Proprietà (B2): stesse chiavi, stessi colori, stesse azioni.
                {
                    AnimMiniTimeline* mini = panel->miniTimeline();
                    check(mini != nullptr && !mini->isHidden(), "la mini linea del tempo si vede con un pezzo animato");
                    mini->resize(300, mini->sizeHint().height());
                    const auto sendMini = [&](QEvent::Type type, const QPointF& p, bool left) {
                        QMouseEvent e(type, p, mini->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                      left ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(mini, &e);
                    };
                    const double firstTime = animation().track(core::AnimParam::Opacity)[0].time;
                    const QPointF m0 = mini->markCenterForTest(0);
                    // Un clic sul rombo porta il cursore sulla chiave.
                    sendMini(QEvent::MouseButtonPress, m0, true);
                    sendMini(QEvent::MouseButtonRelease, m0, false);
                    {
                        QElapsedTimer settle;
                        settle.start();
                        const double want = firstTime - piece.start;
                        while (std::abs(program->position() - want) > 0.05 && settle.elapsed() < 3000)
                            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        check(std::abs(program->position() - want) < 0.1, "un clic sul rombo della mini linea porta il cursore lì");
                    }
                    // Trascinarlo sposta la chiave (fra l'inizio del pezzo e l'altra chiave).
                    const QPointF target(mini->pointForTimeForTest(core::AnimGroup::Opacity, piece.start + 0.2));
                    sendMini(QEvent::MouseButtonPress, mini->markCenterForTest(0), true);
                    sendMini(QEvent::MouseMove, QPointF(mini->markCenterForTest(0).x() + 6, m0.y()), true);
                    sendMini(QEvent::MouseMove, target, true);
                    sendMini(QEvent::MouseButtonRelease, target, false);
                    QCoreApplication::processEvents();
                    check(animation().track(core::AnimParam::Opacity).size() == 2
                              && std::abs(animation().track(core::AnimParam::Opacity)[0].time - (piece.start + 0.2)) < 0.08,
                          "trascinare un rombo della mini linea sposta la chiave");
                    // Clic sul vuoto: il cursore va lì.
                    const QPointF empty = mini->pointForTimeForTest(core::AnimGroup::Opacity, piece.start + 3.0);
                    sendMini(QEvent::MouseButtonPress, empty, true);
                    sendMini(QEvent::MouseButtonRelease, empty, false);
                    {
                        QElapsedTimer settle;
                        settle.start();
                        while (std::abs(program->position() - 3.0) > 0.1 && settle.elapsed() < 3000)
                            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        check(std::abs(program->position() - 3.0) < 0.2, "un clic sul vuoto della mini linea porta il cursore lì");
                    }
                }
                // Il tipo di passaggio (da menu: qui si chiama quello che il menu chiama).
                check(line->keyRow().keys[0].interp == core::Interp::Linear, "all'inizio il passaggio è lineare");
                line->onKeyInterp(line->keyRow().keys[0].group, line->keyRow().keys[0].time, core::Interp::Hold, 0, 0, 1, 1);
                QCoreApplication::processEvents();
                check(line->keyRow().keys[0].interp == core::Interp::Hold
                          && animation().track(core::AnimParam::Opacity)[0].interp == core::Interp::Hold,
                      "il tipo di passaggio cambia sulla chiave e nella riga");
                line->onKeyInterp(line->keyRow().keys[0].group, line->keyRow().keys[0].time, core::Interp::Bezier, 0.34, 1.56, 0.64, 1.0);
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::Opacity)[0].interp == core::Interp::Bezier
                          && std::abs(animation().track(core::AnimParam::Opacity)[0].c1y - 1.56) < 1e-9,
                      "una curva Bézier tiene i suoi punti di controllo");
                // Editor delle curve (K8b): le maniglie si trascinano, i valori e l'elenco delle curve pronte seguono.
                {
                    CurveEditorDialog dialog({0.25, 0.1, 0.25, 1.0});
                    dialog.show();
                    QCoreApplication::processEvents();
                    CurveWidget* cw = dialog.curveWidget();
                    check(cw != nullptr && std::abs(cw->values()[0] - 0.25) < 1e-9 && std::abs(cw->values()[3] - 1.0) < 1e-9,
                          "l'editor delle curve parte dai valori della chiave");
                    if (cw != nullptr) {
                        const auto sendCurve = [&](QEvent::Type type, const QPointF& p) {
                            QMouseEvent e(type, p, cw->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                            QCoreApplication::sendEvent(cw, &e);
                        };
                        const QPointF h2 = cw->handlePosition(1);
                        const QPointF target(h2.x() - cw->width() * 0.3, h2.y() + cw->height() * 0.3);  // più a sinistra e più in basso
                        sendCurve(QEvent::MouseButtonPress, h2);
                        sendCurve(QEvent::MouseMove, QPointF(h2.x() - 5, h2.y() + 5));
                        sendCurve(QEvent::MouseMove, target);
                        sendCurve(QEvent::MouseButtonRelease, target);
                        check(cw->values()[2] < 0.25 && cw->values()[3] < 1.0 && std::abs(cw->values()[0] - 0.25) < 1e-9,
                              "trascinare la seconda maniglia cambia solo x2 e y2");
                        // Fuori dal riquadro i valori si fermano ai limiti.
                        sendCurve(QEvent::MouseButtonPress, cw->handlePosition(0));
                        sendCurve(QEvent::MouseMove, QPointF(-500, -500));
                        sendCurve(QEvent::MouseButtonRelease, QPointF(-500, -500));
                        check(std::abs(cw->values()[0]) < 1e-9 && std::abs(cw->values()[1] - CurveWidget::kMaxY) < 1e-9,
                              "le maniglie non escono dai limiti (x in 0..1, y fra -0,8 e 1,8)");
                        auto* spin = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("curveValue0"));
                        check(spin != nullptr && std::abs(spin->value() - cw->values()[0]) < 0.006, "i valori scritti seguono la curva");
                        if (spin != nullptr) {
                            spin->setValue(0.6);
                            check(std::abs(cw->values()[0] - 0.6) < 1e-9, "scrivere un valore sposta la maniglia");
                        }
                        auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("curvePresets"));
                        check(presets != nullptr && presets->count() > core::bezierPresets().size(), "c'è l'elenco delle curve pronte");
                        if (presets != nullptr && presets->count() > 2) {
                            QMetaObject::invokeMethod(presets, "activated", Q_ARG(int, 2));  // la seconda curva pronta: «Dolce»
                            check(std::abs(cw->values()[0] - 0.42) < 1e-9 && std::abs(cw->values()[3] - 1.0) < 1e-9,
                                  "una curva pronta mette i suoi valori");
                        }
                    }
                    // Dalla chiave: i punti di partenza dipendono dal tipo di passaggio, e con OK la chiave diventa una Bézier.
                    const core::AnimGroup g = core::AnimGroup::Opacity;
                    const double tk = animation().track(core::AnimParam::Opacity)[0].time;
                    window.applyKeyCurve(g, tk, {0.1, 0.9, 0.7, 0.2});
                    QCoreApplication::processEvents();
                    const core::AnimKey& curved = animation().track(core::AnimParam::Opacity)[0];
                    check(curved.interp == core::Interp::Bezier && std::abs(curved.c1y - 0.9) < 1e-9 && std::abs(curved.c2x - 0.7) < 1e-9,
                          "la curva scelta nell'editor finisce sulla chiave");
                    const auto start = window.keyCurve(g, tk);
                    check(std::abs(start[1] - 0.9) < 1e-9 && std::abs(start[2] - 0.7) < 1e-9, "riaprendo l'editor si riparte dalla curva della chiave");
                    window.undo();
                    QCoreApplication::processEvents();
                }
                // Canc: la chiave scelta si toglie (e non il pezzo).
                const QPointF last = line->keyCenterForTest(1);
                send(QEvent::MouseButtonPress, last, true);
                send(QEvent::MouseButtonRelease, last, false);
                window.deleteSelection();
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::Opacity).size() == 1 && window.project().sequence().fragments().size() == 1,
                      "Canc toglie la chiave scelta e non il pezzo");
                check(line->keyRow().keys.size() == 1, "la riga dei rombi segue");
                // Annulla riporta la chiave.
                window.undo();
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::Opacity).size() == 2, "Annulla riporta la chiave");
                // Più gruppi animati (K8): una sotto-riga e un colore per gruppo, ogni operazione vale per il gruppo cliccato.
                {
                    const double tOpacity = animation().track(core::AnimParam::Opacity)[0].time;
                    window.editAnimationKeys(QStringLiteral("prova gruppi"), [&](core::TransformAnimation& a, core::Transform&) {
                        for (const core::AnimParam p : {core::AnimParam::ScaleX, core::AnimParam::ScaleY}) {
                            core::setKey(a, p, {.time = tOpacity, .value = 1.0});
                            core::setKey(a, p, {.time = piece.start + 2.6, .value = 1.5});
                        }
                        return true;
                    });
                    QCoreApplication::processEvents();
                    int scaleMarks = 0;
                    int opacityMarks = 0;
                    int scaleIndexAtOpacityTime = -1;
                    int opacityIndex = -1;
                    for (int i = 0; i < static_cast<int>(line->keyRow().keys.size()); ++i) {
                        const auto& mark = line->keyRow().keys[static_cast<std::size_t>(i)];
                        scaleMarks += mark.group == core::AnimGroup::Scale;
                        opacityMarks += mark.group == core::AnimGroup::Opacity;
                        if (std::abs(mark.time - tOpacity) < 1e-6) {
                            if (mark.group == core::AnimGroup::Scale)
                                scaleIndexAtOpacityTime = i;
                            if (mark.group == core::AnimGroup::Opacity)
                                opacityIndex = i;
                        }
                    }
                    check(scaleMarks == 2 && opacityMarks == 2, "ogni gruppo ha i suoi rombi nella riga");
                    check(scaleIndexAtOpacityTime >= 0 && opacityIndex >= 0, "nello stesso istante ci sono un rombo di scala e uno di opacità");
                    if (scaleIndexAtOpacityTime >= 0 && opacityIndex >= 0) {
                        const QPointF a = line->keyCenterForTest(scaleIndexAtOpacityTime);
                        const QPointF b = line->keyCenterForTest(opacityIndex);
                        check(std::abs(a.x() - b.x()) < 1.0 && std::abs(a.y() - b.y()) > 4.0,
                              "i rombi dello stesso istante stanno su sotto-righe diverse");
                        // Clic sul rombo dell'opacità: sceglie quel gruppo; Canc toglie solo l'opacità di quell'istante.
                        send(QEvent::MouseButtonPress, b, true);
                        send(QEvent::MouseButtonRelease, b, false);
                        check(line->selectedKeyGroup() == core::AnimGroup::Opacity, "il clic sceglie il rombo del gruppo cliccato");
                        const std::size_t scaleKeys = animation().track(core::AnimParam::ScaleX).size();
                        window.deleteSelection();
                        QCoreApplication::processEvents();
                        check(animation().track(core::AnimParam::Opacity).size() == 1 && animation().track(core::AnimParam::ScaleX).size() == scaleKeys,
                              "Canc toglie la chiave del gruppo scelto e lascia le altre");
                        window.undo();  // riporta l'opacità
                        QCoreApplication::processEvents();
                    }
                    // Via le chiavi di scala di prova (un solo passo, senza toccare l'opacità).
                    window.editAnimationKeys(QStringLiteral("pulizia"), [&](core::TransformAnimation& a, core::Transform&) {
                        return core::clearTrack(a, core::AnimParam::ScaleX) | core::clearTrack(a, core::AnimParam::ScaleY);
                    });
                    QCoreApplication::processEvents();
                    check(!animation().animated(core::AnimParam::ScaleX) && animation().track(core::AnimParam::Opacity).size() == 2,
                          "dopo la pulizia restano solo le chiavi dell'opacità");
                }
                // Un gesto sul monitor (K6): il parametro animato che cambia riceve una chiave sul cursore, gli altri cambiano la base.
                {
                    seek(2.0);
                    const core::Fragment& now = *window.project().sequence().find(id);
                    const core::Transform shown = core::evaluateTransform(now.transform, now.animation, piece.start + 2.0);
                    core::Transform edited = shown;
                    edited.opacity = 0.7;   // animato (ha le sue chiavi)
                    edited.rotation = 12.0;  // fisso
                    const std::size_t keysBefore = animation().track(core::AnimParam::Opacity).size();
                    check(window.commitMonitorTransform(id, edited), "il gesto sul monitor di un pezzo animato viene accettato");
                    QCoreApplication::processEvents();
                    const core::Fragment& after = *window.project().sequence().find(id);
                    check(animation().track(core::AnimParam::Opacity).size() == keysBefore + 1
                              && after.animation.track(core::AnimParam::Opacity).back().value == 0.7,
                          "il parametro animato cambiato dal gesto riceve una chiave sul cursore");
                    check(!after.animation.animated(core::AnimParam::Rotation) && std::abs(after.transform.rotation - 12.0) < 1e-9,
                          "il parametro fisso cambiato dal gesto cambia la trasformazione di base");
                    check(std::abs(after.transform.opacity - 1.0) < 1e-9, "la base del parametro animato non cambia");
                    window.undo();
                    QCoreApplication::processEvents();
                    check(animation().track(core::AnimParam::Opacity).size() == keysBefore
                              && std::abs(window.project().sequence().find(id)->transform.rotation) < 1e-9,
                          "il gesto sul monitor è un solo passo di Annulla");
                }
                // Un punto del percorso trascinato sul monitor cambia la posizione della chiave (e un solo passo di Annulla).
                {
                    const double tk = piece.start + 1.0;
                    window.editAnimationKeys(QStringLiteral("prova percorso"), [&](core::TransformAnimation& a, core::Transform&) {
                        for (const core::AnimParam p : {core::AnimParam::PosX, core::AnimParam::PosY}) {
                            core::setKey(a, p, {.time = piece.start + 0.2, .value = 0.0});
                            core::setKey(a, p, {.time = tk, .value = 50.0});
                        }
                        return true;
                    });
                    QCoreApplication::processEvents();
                    window.programMonitor()->onMotionKeyMoved(false, id, tk, 300.0, -80.0);
                    QCoreApplication::processEvents();
                    const core::AnimKey* kx = core::findKey(animation(), core::AnimParam::PosX, tk);
                    const core::AnimKey* ky = core::findKey(animation(), core::AnimParam::PosY, tk);
                    check(kx != nullptr && ky != nullptr && std::abs(kx->value - 300.0) < 1e-9 && std::abs(ky->value + 80.0) < 1e-9,
                          "un punto del percorso trascinato cambia X e Y della chiave");
                    window.undo();
                    QCoreApplication::processEvents();
                    const core::AnimKey* back = core::findKey(animation(), core::AnimParam::PosX, tk);
                    check(back != nullptr && std::abs(back->value - 50.0) < 1e-9, "il punto del percorso è un solo passo di Annulla");
                }
                // Preset di animazione dal menu del pannello (K7).
                {
                    auto* presets = panel->findChild<QToolButton*>(QStringLiteral("presetAnimazione"));
                    check(presets != nullptr && presets->isEnabled() && presets->menu() != nullptr
                              && presets->menu()->actions().size() >= core::kAnimPresetCount,
                          "il pannello ha il menu dei preset di animazione con un pezzo scelto");
                    if (presets != nullptr && presets->menu() != nullptr) {
                        QAction* zoom = nullptr;
                        for (QAction* a : presets->menu()->actions()) {
                            if (a->text() == QString::fromUtf8(core::animPresetName(core::AnimPreset::ZoomIn)))
                                zoom = a;
                        }
                        check(zoom != nullptr, "c'è il preset «Zoom lento avanti»");
                        if (zoom != nullptr) {
                            zoom->trigger();
                            QCoreApplication::processEvents();
                            const auto& scale = animation().track(core::AnimParam::ScaleX);
                            check(scale.size() == 2 && std::abs(scale[0].time - piece.start) < 1e-6 && std::abs(scale[1].time - piece.end) < 1e-6
                                      && std::abs(scale[1].value - 1.15) < 1e-9,
                                  "il preset mette le chiavi di scala all'inizio e alla fine del pezzo");
                            check(line->keyRow().keys.size() >= 2, "i rombi del preset si vedono sulla linea del tempo");
                            window.undo();
                            QCoreApplication::processEvents();
                            check(!animation().animated(core::AnimParam::ScaleX), "il preset è un solo passo di Annulla");
                        }
                    }
                }
                // Un pezzo non scelto (selezione vuota): la riga sparisce.
                line->selectObjects({});
                QCoreApplication::processEvents();
                check(line->keyRow().keys.empty(), "senza un pezzo scelto non c'è la riga dei rombi");
            }

            // --- La sezione «Trasforma» nella pagina della scritta (S3): stessi controlli, senza ritaglio, tempi dall'inizio della scritta.
            {
                const int textId = window.addTextClip(false);
                QCoreApplication::processEvents();
                const core::TextClip* text = window.project().textTracks().find(textId);
                check(textId != 0 && text != nullptr, "una scritta nuova nella sequenza");
                if (text != nullptr && text->length >= 2.0) {
                    const double start = text->position;
                    const auto clipNow = [&]() -> const core::TextClip& { return *window.project().textTracks().find(textId); };
                    auto* rotation = panel->findChild<QDoubleSpinBox*>(QStringLiteral("rotazione"));
                    auto* cropLeft = panel->findChild<QDoubleSpinBox*>(QStringLiteral("ritaglioSinistra"));
                    auto* rotationKey = panel->findChild<QToolButton*>(QStringLiteral("chiave2"));  // gruppo Rotazione
                    check(panel->showingText() && window.textPanel()->isVisibleTo(panel), "con una scritta scelta il pannello mostra il suo editor");
                    check(rotation != nullptr && rotation->isVisibleTo(panel) && rotationKey != nullptr && rotationKey->isVisibleTo(panel),
                          "in fondo all'editor ci sono i controlli della trasformazione (rotazione e il suo rombo)");
                    check(cropLeft != nullptr && !cropLeft->isVisibleTo(panel), "per le scritte il ritaglio non c'è");
                    if (rotation != nullptr && rotationKey != nullptr) {
                        // Un valore cambiato senza animazione cambia la trasformazione fissa della scritta.
                        seek(start + 0.5);
                        rotation->setValue(30.0);
                        QCoreApplication::processEvents();
                        check(std::abs(clipNow().transform.rotation - 30.0) < 1e-9 && clipNow().animation.empty(),
                              "la rotazione dal pannello cambia la trasformazione della scritta");
                        // Il rombo mette una chiave sul cursore: tempo dall'inizio della scritta.
                        seek(start + 1.0);
                        rotationKey->click();
                        QCoreApplication::processEvents();
                        const auto& keys = clipNow().animation.track(core::AnimParam::Rotation);
                        check(keys.size() == 1 && std::abs(keys[0].time - 1.0) < 0.1 && std::abs(keys[0].value - 30.0) < 1e-6,
                              "il rombo mette una chiave al secondo giusto dall'inizio della scritta");
                        check(panel->miniTimeline() != nullptr && !panel->miniTimeline()->isHidden(),
                              "la mini linea del tempo mostra le chiavi della scritta");
                        // Con la chiave, cambiare il valore in un altro istante mette un'altra chiave (auto-key).
                        seek(start + 2.0);
                        rotation->setValue(90.0);
                        QCoreApplication::processEvents();
                        check(clipNow().animation.track(core::AnimParam::Rotation).size() == 2, "con l'animazione una modifica mette la chiave sul cursore");
                        // Modificare il testo non tocca la trasformazione né fa una modifica in più.
                        core::TextClip edited = clipNow();
                        edited.text = "Altro testo";
                        const core::TransformAnimation animBefore = clipNow().animation;
                        const bool ok = window.applyTextEdit(edited, nullptr, false);
                        QCoreApplication::processEvents();
                        check(ok && clipNow().text == "Altro testo" && clipNow().animation == animBefore,
                              "cambiare il testo lascia l'animazione com'è");
                        // Annulla toglie l'ultima chiave, un passo alla volta.
                        window.undo();  // il testo
                        window.undo();  // la chiave a 2 s
                        QCoreApplication::processEvents();
                        check(clipNow().animation.track(core::AnimParam::Rotation).size() == 1, "Annulla toglie una chiave alla volta");
                    }
                    // --- Le animazioni pronte per le scritte (S5): entrate e uscite, senza le panoramiche.
                    {
                        auto* presets = panel->findChild<QToolButton*>(QStringLiteral("presetAnimazione"));
                        check(presets != nullptr && presets->isVisibleTo(panel) && presets->isEnabled(), "con una scritta scelta c'è il menu delle animazioni pronte");
                        if (presets != nullptr && presets->menu() != nullptr) {
                            QAction* slide = nullptr;
                            bool panVisible = false;
                            for (QAction* act : presets->menu()->actions()) {
                                if (act->text() == QString::fromUtf8(core::animPresetName(core::AnimPreset::SlideInLeft)))
                                    slide = act;
                                if (act->text() == QString::fromUtf8(core::animPresetName(core::AnimPreset::PanLeft)) && act->isVisible())
                                    panVisible = true;
                            }
                            check(slide != nullptr && !panVisible, "per le scritte ci sono le entrate e non le panoramiche");
                            if (slide != nullptr) {
                                const std::size_t posKeys = clipNow().animation.track(core::AnimParam::PosX).size();
                                slide->trigger();
                                QCoreApplication::processEvents();
                                const auto& px = clipNow().animation.track(core::AnimParam::PosX);
                                check(posKeys == 0 && px.size() == 2 && std::abs(px[0].time) < 1e-9 && px[0].value < -100.0,
                                      "«Entra da sinistra» mette le chiavi all'inizio della scritta (tempi dall'inizio), partendo da fuori a sinistra");
                                window.undo();
                                QCoreApplication::processEvents();
                                check(!clipNow().animation.animated(core::AnimParam::PosX), "l'animazione pronta della scritta è un solo passo di Annulla");
                            }
                        }
                    }
                    // --- Una sola posizione per la scritta: quella vera nel fotogramma, in % (centro di stile + spostamento).
                    {
                        QDoubleSpinBox* posX = nullptr;
                        QDoubleSpinBox* posY = nullptr;
                        for (QDoubleSpinBox* spin : panel->findChildren<QDoubleSpinBox*>(QStringLiteral("posizioneX")))
                            posX = spin->isVisibleTo(panel) ? spin : posX;
                        for (QDoubleSpinBox* spin : panel->findChildren<QDoubleSpinBox*>(QStringLiteral("posizioneY")))
                            posY = spin->isVisibleTo(panel) ? spin : posY;
                        check(posX != nullptr && posY != nullptr, "la scritta ha una sola posizione visibile (X e Y)");
                        auto* grid = window.textPanel()->findChild<QToolButton*>(QStringLiteral("posto00"));  // in alto a sinistra: 20 %, 12 %
                        if (posX != nullptr && posY != nullptr && grid != nullptr) {
                            const double frameW = window.project().format().video()->width;
                            const double frameH = window.project().format().video()->height;
                            seek(start + 0.5);
                            const double cy = clipNow().style.centerY;
                            check(std::abs(posX->value() - (clipNow().style.centerX + clipNow().transform.posX / frameW) * 100.0) < 0.2
                                      && std::abs(posY->value() - (cy + clipNow().transform.posY / frameH) * 100.0) < 0.2,
                                  "la posizione mostrata è centro di stile più spostamento, in % del fotogramma");
                            grid->click();
                            QCoreApplication::processEvents();
                            check(std::abs(clipNow().style.centerX - 0.2) < 1e-9 && std::abs(clipNow().style.centerY - 0.12) < 1e-9
                                      && std::abs(clipNow().transform.posX) < 1e-9 && std::abs(clipNow().transform.posY) < 1e-9,
                                  "un punto fisso mette la posizione nel centro di stile e azzera lo spostamento");
                            check(std::abs(posX->value() - 20.0) < 0.2 && std::abs(posY->value() - 12.0) < 0.2, "il pannello mostra 20 % e 12 %");
                            posX->setValue(35.0);
                            QCoreApplication::processEvents();
                            check(std::abs(clipNow().style.centerX - 0.35) < 1e-9 && std::abs(clipNow().transform.posX) < 1e-9,
                                  "scrivere la X nel campo cambia la posizione (non animata)");
                            // Con la posizione animata la X scritta nel campo mette una chiave sul cursore (spostamento dal centro di stile).
                            window.editAnimationKeys(QStringLiteral("prova posizione"), [&](core::TransformAnimation& a, core::Transform&) {
                                core::setKey(a, core::AnimParam::PosX, {.time = 0.2, .value = 0.0});
                                core::setKey(a, core::AnimParam::PosX, {.time = 2.0, .value = 100.0});
                                return true;
                            });
                            seek(start + 1.0);
                            posX->setValue(60.0);
                            QCoreApplication::processEvents();
                            const core::AnimKey* k = core::findKey(clipNow().animation, core::AnimParam::PosX, 1.0);
                            check(k != nullptr && std::abs(k->value - (0.60 - 0.35) * frameW) < 0.5 && std::abs(clipNow().style.centerX - 0.35) < 1e-9,
                                  "con la posizione animata la X scritta mette la chiave sul cursore");
                            window.undo();
                            window.undo();
                            window.undo();
                            QCoreApplication::processEvents();
                        }
                    }
                    // --- La riga dei rombi sotto la scritta e le maniglie sul monitor (S4).
                    {
                        SequenceTimeline* line = window.sequenceTimeline();
                        check(line->keyRow().text && line->keyRow().id == textId && !line->keyRow().keys.empty(),
                              "con una scritta animata scelta la linea del tempo ha la sua riga di rombi");
                        program->setTransformMode(true);
                        seek(start + 1.5);
                        check(program->manipulatorsVisible(), "con la scritta scelta e sotto il cursore si vedono le maniglie");
                        // Un gesto finito sul monitor: la posizione cambia nella base (non è animata), la rotazione animata prende una chiave.
                        core::Transform shown = core::evaluateTransform(clipNow().transform, clipNow().animation, 1.5);
                        core::Transform edited = shown;
                        edited.posX = 40.0;
                        edited.rotation = 45.0;
                        check(window.commitMonitorTextTransform(textId, edited), "il gesto sul monitor di una scritta viene accettato");
                        QCoreApplication::processEvents();
                        check(std::abs(clipNow().transform.posX - 40.0) < 1e-9 && !clipNow().animation.animated(core::AnimParam::PosX),
                              "la posizione non animata cambia la trasformazione della scritta");
                        check(clipNow().animation.track(core::AnimParam::Rotation).size() == 2
                                  && std::abs(core::findKey(clipNow().animation, core::AnimParam::Rotation, 1.5)->value - 45.0) < 1e-6,
                              "la rotazione animata riceve la chiave sul cursore (secondi dall'inizio della scritta)");
                        window.undo();
                        QCoreApplication::processEvents();
                        check(std::abs(clipNow().transform.posX) < 1e-9 && clipNow().animation.track(core::AnimParam::Rotation).size() == 1,
                              "il gesto sulla scritta è un solo passo di Annulla");
                        // Il percorso: due chiavi di posizione, le maniglie e i punti si vedono.
                        window.editAnimationKeys(QStringLiteral("prova percorso scritta"), [&](core::TransformAnimation& a, core::Transform&) {
                            for (const core::AnimParam p : {core::AnimParam::PosX, core::AnimParam::PosY}) {
                                core::setKey(a, p, {.time = 0.5, .value = 0.0});
                                core::setKey(a, p, {.time = 2.0, .value = 100.0});
                            }
                            return true;
                        });
                        QCoreApplication::processEvents();
                        const auto path = program->motionPath();
                        check(path.has_value() && path->text && path->dots.size() >= 2 && std::abs(path->dots.back().fileTime - 2.0) < 1e-9,
                              "con la posizione della scritta animata il monitor ha il percorso del movimento");
                        program->onTextMotionKeyMoved(textId, 2.0, 160.0, 20.0);  // (quello che il monitor manda al rilascio)
                        QCoreApplication::processEvents();
                        const core::AnimKey* kx = core::findKey(clipNow().animation, core::AnimParam::PosX, 2.0);
                        const core::AnimKey* ky = core::findKey(clipNow().animation, core::AnimParam::PosY, 2.0);
                        check(kx != nullptr && ky != nullptr && std::abs(kx->value - 160.0) < 1e-9 && std::abs(ky->value - 20.0) < 1e-9,
                              "un punto del percorso della scritta cambia X e Y della chiave");
                        window.undo();
                        QCoreApplication::processEvents();
                        const core::AnimKey* back = core::findKey(clipNow().animation, core::AnimParam::PosX, 2.0);
                        check(back != nullptr && std::abs(back->value - 100.0) < 1e-9, "il punto del percorso della scritta è un solo passo di Annulla");
                        program->setTransformMode(false);
                    }
                    // Scegliendo di nuovo il pezzo video torna la sua pagina, con il ritaglio.
                    window.sequenceTimeline()->selectObjects({{core::ObjectRef::Kind::Piece, id}});
                    QCoreApplication::processEvents();
                    check(panel->showingTransform() && cropLeft != nullptr && cropLeft->isVisibleTo(panel),
                          "tornando a un pezzo video si rivede la sua pagina con il ritaglio");
                }
            }
        }
    }

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
