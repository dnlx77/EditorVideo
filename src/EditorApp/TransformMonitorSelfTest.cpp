#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/PropertiesPanel.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/SelfTests.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/TransformManip.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QElapsedTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QToolButton>

#include <algorithm>
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
            QDoubleSpinBox* posX = nullptr;  // (la pagina delle scritte ha un campo con lo stesso nome: si prende quello visibile)
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
                const double t3 = piece.start + 2.5;
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
                // Il tipo di passaggio (da menu: qui si chiama quello che il menu chiama).
                check(line->keyRow().keys[0].interp == core::Interp::Linear, "all'inizio il passaggio è lineare");
                line->onKeyInterp(line->keyRow().keys[0].time, core::Interp::Hold, 0, 0, 1, 1);
                QCoreApplication::processEvents();
                check(line->keyRow().keys[0].interp == core::Interp::Hold
                          && animation().track(core::AnimParam::Opacity)[0].interp == core::Interp::Hold,
                      "il tipo di passaggio cambia sulla chiave e nella riga");
                line->onKeyInterp(line->keyRow().keys[0].time, core::Interp::Bezier, 0.34, 1.56, 0.64, 1.0);
                QCoreApplication::processEvents();
                check(animation().track(core::AnimParam::Opacity)[0].interp == core::Interp::Bezier
                          && std::abs(animation().track(core::AnimParam::Opacity)[0].c1y - 1.56) < 1e-9,
                      "una curva Bézier tiene i suoi punti di controllo");
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
                // Un pezzo non scelto (selezione vuota): la riga sparisce.
                line->selectObjects({});
                QCoreApplication::processEvents();
                check(line->keyRow().keys.empty(), "senza un pezzo scelto non c'è la riga dei rombi");
            }
        }
    }

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
