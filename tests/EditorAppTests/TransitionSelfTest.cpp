#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "SelfTests.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TestAccess.h"
#include "EditorApp/TransitionMime.h"
#include "EditorApp/TransitionListPanel.h"
#include "EditorApp/TransitionPreview.h"
#include "EditorApp/TransitionPreviewWidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QMimeData>
#include <QtCore/QStringList>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTabWidget>

#include <cmath>
#include <cstdio>

namespace editor::app {

namespace core = editor::core;

// --transition-test: l'elenco delle transizioni (una voce per ogni transizione del registro, con la sua miniatura), la
// scelta di una voce e le due schede del blocco in alto a sinistra ("File del progetto" come prima e "Transizioni").
// Codice 0 se tutto è giusto, 3 se un controllo non riesce.
int runTransitionTest(const QStringList&, qsizetype)
{
    SelfCheck check;
    std::printf("Prova dell'elenco delle transizioni\n");

    const auto& registry = core::transitionRegistry();
    TransitionListPanel panel;
    check(panel.count() == static_cast<int>(registry.size()), "una voce per ogni transizione del registro");
    bool order = true;
    bool icons = true;
    for (int i = 0; i < panel.count() && i < static_cast<int>(registry.size()); ++i) {
        const QListWidgetItem* item = panel.list()->item(i);
        order = order && item->data(Qt::UserRole).toString() == QString::fromUtf8(registry[i]->id())
                && item->text() == QString::fromUtf8(registry[i]->name());
        icons = icons && !item->icon().isNull();
    }
    check(order, "voci nell'ordine del registro, con nome e id giusti");
    check(icons, "ogni voce ha la miniatura");

    // Le miniature sono quelle a metà effetto delle immagini di esempio e fra loro diverse.
    bool distinct = true;
    const QImage first = renderTransitionSample(*registry[0], {}, 0.5, QSize(96, 54));
    for (std::size_t i = 1; i < registry.size(); ++i)
        distinct = distinct && renderTransitionSample(*registry[i], {}, 0.5, QSize(96, 54)) != first;
    check(distinct, "le miniature delle transizioni sono diverse fra loro");
    const QImage start = renderTransitionSample(*registry[0], {}, 0.0, QSize(64, 36));
    const QImage end = renderTransitionSample(*registry[0], {}, 1.0, QSize(64, 36));
    check(start == transitionSampleFrom(QSize(64, 36)) && end == transitionSampleTo(QSize(64, 36)),
          "a 0 si vede l'immagine A e a 1 la B");

    const core::Transition* chosen = nullptr;
    int calls = 0;
    panel.onSelected = [&](const core::Transition* t) {
        chosen = t;
        ++calls;
    };
    check(panel.select("dissolvenza-nero") && chosen == core::findTransition("dissolvenza-nero") && calls == 1
              && panel.selected() == chosen,
          "scegliendo una voce arriva la transizione giusta");
    check(!panel.select("non-esiste") && panel.selected() == chosen, "un id che non c'è non cambia la scelta");

    // L'anteprima animata: il ciclo (A ferma, transizione, B ferma) e i suoi fotogrammi.
    const double cycle = TransitionPreviewWidget::cycleSeconds();
    check(TransitionPreviewWidget::progressAt(0.0) == 0.0 && TransitionPreviewWidget::progressAt(cycle - 0.01) == 1.0
              && TransitionPreviewWidget::progressAt(cycle) == 0.0 && TransitionPreviewWidget::progressAt(cycle + 100.0) <= 1.0,
          "il ciclo parte da A, arriva a B e ricomincia");
    double previous = 0.0;
    bool rising = true;
    for (double t = 0.0; t < cycle; t += 0.05) {
        const double p = TransitionPreviewWidget::progressAt(t);
        rising = rising && p >= previous && p >= 0.0 && p <= 1.0;
        previous = p;
    }
    check(rising, "durante il ciclo l'avanzamento non scende mai");
    TransitionPreviewWidget preview;
    check(preview.frameAt(0.0).isNull() && preview.transition() == nullptr, "senza transizione non c'è nessun fotogramma");
    const core::Transition* crossfade = core::findTransition("dissolvenza");
    preview.setTransition(crossfade);
    const QSize sampleSize = preview.frameAt(0.0).size();
    check(preview.transition() == crossfade && !sampleSize.isEmpty(), "con una transizione si hanno i fotogrammi");
    check(preview.frameAt(0.0) == transitionSampleFrom(sampleSize), "all'inizio del ciclo si vede l'immagine A");
    check(preview.frameAt(cycle - 0.01) == transitionSampleTo(sampleSize), "alla fine del ciclo si vede l'immagine B");
    const double middle = (cycle - 0.0) / 2.0;  // metà ciclo = metà transizione (A e B ferme per lo stesso tempo)
    check(preview.frameAt(middle) == renderTransitionSample(*crossfade, {}, 0.5, sampleSize),
          "a metà ciclo il fotogramma è quello a metà transizione");
    preview.setTime(middle);
    check(std::abs(preview.time() - middle) < 1e-9, "il punto del ciclo mostrato si può impostare");

    // Trascinare dall'elenco: il trascinamento porta l'id della transizione.
    {
        QMimeData* mime = panel.list()->model()->mimeData({panel.list()->model()->index(2, 0)});
        check(mime != nullptr && mime->hasFormat(QLatin1String(kTransitionMime))
                  && QString::fromUtf8(mime->data(QLatin1String(kTransitionMime))) == QString::fromUtf8(registry[2]->id()),
              "il trascinamento dall'elenco porta l'id della transizione");
        delete mime;
    }

    // La linea del tempo: due pezzi attaccati da 4 s l'uno, la giunzione a 4 s.
    {
        SequenceTimeline line;
        line.resize(1000, 320);
        line.show();
        QCoreApplication::processEvents();
        SequenceTimeline::Block a;
        a.fragmentId = 11;
        a.label = QStringLiteral("a.mp4");
        a.length = 4.0;
        a.clipId = 1;
        SequenceTimeline::Block b = a;
        b.fragmentId = 12;
        b.sourceStart = 10.0;
        line.setBlocks({a, b});
        line.fitAll();
        QCoreApplication::processEvents();
        const double junctionX = TestAccess::xForJunction(line, 0);
        const double y = TestAccess::videoLaneCenter(line);
        check(std::abs(junctionX - TestAccess::xForSequenceTime(line, 4.0)) < 1.0, "la giunzione è alla fine del primo pezzo");

        // Si lascia sulla giunzione: arriva il tipo e il pezzo prima; lontano dalla giunzione: rifiutata.
        QString droppedType;
        int droppedAt = -1;
        int refusedAt = -1;
        bool allow = true;
        line.canDropTransition = [&](int) { return allow; };
        line.onTransitionDropped = [&](const QString& type, int fragment) {
            droppedType = type;
            droppedAt = fragment;
        };
        line.onTransitionDropRefused = [&](int fragment) { refusedAt = fragment; };
        const auto dropAt = [&](double x) {
            QMimeData mimeData;
            mimeData.setData(QLatin1String(kTransitionMime), QByteArrayLiteral("scorrimento"));
            const QPoint at(static_cast<int>(x), static_cast<int>(y));
            QDragEnterEvent enter(at, Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&line, &enter);
            const int hovered = TestAccess::transitionDropBlock(line);
            QDropEvent drop(QPointF(at), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&line, &drop);
            return hovered;
        };
        const int onJunction = dropAt(junctionX + 6);
        check(onJunction == 0 && droppedType == QStringLiteral("scorrimento") && droppedAt == 11 && refusedAt == -1,
              "lasciata sulla giunzione: arrivano il tipo e il pezzo prima");
        droppedAt = -1;
        const int far = dropAt(TestAccess::xForSequenceTime(line, 1.0));
        check(far == -1 && droppedAt == -1 && refusedAt == 0, "lasciata lontano da una giunzione: rifiutata");
        allow = false;
        refusedAt = -1;
        dropAt(junctionX);
        check(droppedAt == -1 && refusedAt == 11, "giunzione che non ammette la transizione: rifiutata con il pezzo");
        allow = true;

        // Il blocco della transizione sta a cavallo della giunzione.
        line.setTransitions({{5, QStringLiteral("Dissolvenza incrociata"), 3.0, 5.0, 4.0}});
        const QRectF block = TestAccess::transitionRectForTest(line, 5);
        check(std::abs(block.left() - TestAccess::xForSequenceTime(line, 3.0)) < 1.0 && std::abs(block.right() - TestAccess::xForSequenceTime(line, 5.0)) < 1.0
                  && std::abs(block.center().x() - junctionX) < 1.0,
              "il blocco della transizione è a cavallo della giunzione");

        const auto mouse = [&](QEvent::Type type, double x, Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(x, y), line.mapToGlobal(QPointF(x, y)),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&line, &e);
        };
        int selectedCallback = 0;
        line.onTransitionSelected = [&](int id) { selectedCallback = id; };
        mouse(QEvent::MouseButtonPress, junctionX, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, junctionX, Qt::NoButton);
        check(line.selectedTransition() == 5 && selectedCallback == 5 && line.selected() == 0,
              "un clic sul blocco seleziona la transizione, non il pezzo sotto");

        // Il bordo destro trascinato cambia la durata (limitata); lasciare senza muovere non cambia niente.
        int changedId = 0;
        double changedTo = 0.0;
        int callCount = 0;
        line.transitionLimit = [](int, double desired) { return std::clamp(desired, 0.1, 3.0); };
        line.onTransitionDuration = [&](int id, double duration) {
            changedId = id;
            changedTo = duration;
            ++callCount;
        };
        mouse(QEvent::MouseButtonPress, block.right(), Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, block.right(), Qt::NoButton);
        check(callCount == 0, "un clic sul bordo senza trascinare non cambia la durata");
        const double perSecond = TestAccess::xForSequenceTime(line, 1.0) - TestAccess::xForSequenceTime(line, 0.0);
        mouse(QEvent::MouseButtonPress, block.right(), Qt::LeftButton);
        mouse(QEvent::MouseMove, block.right() + 0.25 * perSecond, Qt::LeftButton);
        mouse(QEvent::MouseMove, block.right() + 0.5 * perSecond, Qt::LeftButton);
        const QRectF during = TestAccess::transitionRectForTest(line, 5);
        mouse(QEvent::MouseButtonRelease, block.right() + 0.5 * perSecond, Qt::NoButton);
        check(callCount == 1 && changedId == 5 && std::abs(changedTo - 3.0) < 0.05 && std::abs(during.width() - 3.0 * perSecond) < 2.0,
              "trascinando il bordo la durata diventa il doppio della distanza dalla giunzione (3 s)");
        mouse(QEvent::MouseButtonPress, block.right(), Qt::LeftButton);
        mouse(QEvent::MouseMove, junctionX + 0.02 * perSecond, Qt::LeftButton);
        mouse(QEvent::MouseMove, junctionX + 0.01 * perSecond, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, junctionX + 0.01 * perSecond, Qt::NoButton);
        check(callCount == 2 && std::abs(changedTo - 0.1) < 1e-9, "la durata non scende sotto il minimo");

        // Doppio clic: si apre; un clic su un pezzo toglie la selezione della transizione.
        int activated = 0;
        line.onTransitionActivate = [&](int id) { activated = id; };
        {
            QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(junctionX, y), line.mapToGlobal(QPointF(junctionX, y)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&line, &e);
        }
        check(activated == 5, "il doppio clic sul blocco apre la transizione");
        const double awayX = TestAccess::xForSequenceTime(line, 0.5);
        mouse(QEvent::MouseButtonPress, awayX, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, awayX, Qt::NoButton);
        check(line.selectedTransition() == 0 && line.selected() == 11, "un clic su un pezzo toglie la selezione della transizione");
        line.setSelectedTransition(5);
        line.setSelected(12);
        check(line.selectedTransition() == 0, "scegliere un pezzo toglie la selezione della transizione");
        line.setSelectedTransition(5);
        line.setTransitions({});
        check(line.selectedTransition() == 0, "se la transizione sparisce, la selezione sparisce");

        // Il badge «fx» dei pezzi trasformati: solo dove c'è una trasformazione; un clic sceglie il pezzo e lo dice.
        SequenceTimeline::Block fx = a;
        fx.transformNote = QStringLiteral("Scala 150%");
        line.setBlocks({fx, b});
        QCoreApplication::processEvents();
        check(!TestAccess::transformBadgeRectForTest(line, 11).isEmpty() && TestAccess::transformBadgeRectForTest(line, 12).isEmpty(),
              "il badge c'è solo sul pezzo con una trasformazione");
        int badgeClicked = 0;
        int badgeSelected = 0;
        line.onTransformBadge = [&](int id) { badgeClicked = id; };
        line.onSelect = [&](int id) { badgeSelected = id; };
        const QPointF badgeCenter = TestAccess::transformBadgeRectForTest(line, 11).center();
        {
            QMouseEvent press(QEvent::MouseButtonPress, badgeCenter, line.mapToGlobal(badgeCenter), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&line, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, badgeCenter, line.mapToGlobal(badgeCenter), Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QCoreApplication::sendEvent(&line, &release);
        }
        check(badgeClicked == 11 && badgeSelected == 11 && line.selected() == 11, "un clic sul badge sceglie il pezzo e mostra la sua trasformazione");
    }

    // Le due schede nella finestra.
    MainWindow window;
    QTabWidget* tabs = TestAccess::leftTabs(window);
    check(tabs != nullptr && tabs->count() >= 2 && tabs->tabText(0) == QStringLiteral("File del progetto")
              && tabs->tabText(1) == QStringLiteral("Transizioni"),
          "il blocco in alto a sinistra ha le schede File del progetto e Transizioni");
    check(tabs != nullptr && tabs->currentIndex() == 0 && TestAccess::filesDock(window)->widget() == tabs,
          "all'inizio è aperta la scheda dei file, nel pannello dei file");
    check(TestAccess::transitionPanel(window) != nullptr && tabs != nullptr && tabs->widget(1) == TestAccess::transitionPanel(window)
              && TestAccess::transitionPanel(window)->count() == static_cast<int>(registry.size()),
          "la scheda Transizioni ha l'elenco completo");

    // Scegliendo una transizione la Sorgente ne mostra l'anteprima; il file scelto riporta il lettore.
    check(TestAccess::sourceStack(window) != nullptr && TestAccess::sourceStack(window)->currentWidget() == window.player(),
          "all'inizio la Sorgente mostra il lettore");
    TestAccess::transitionPanel(window)->select("scorrimento");
    check(TestAccess::sourceStack(window)->currentWidget() == TestAccess::transitionPreview(window)
              && TestAccess::transitionPreview(window)->transition() == core::findTransition("scorrimento"),
          "scegliendo una transizione la Sorgente ne mostra l'anteprima");
    TestAccess::transitionPanel(window)->select("dissolvenza");
    check(TestAccess::transitionPreview(window)->transition() == crossfade, "la nuova scelta cambia l'anteprima");
    window.showSourcePlayer();
    check(TestAccess::sourceStack(window)->currentWidget() == window.player(), "scegliendo un file torna il lettore");

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
