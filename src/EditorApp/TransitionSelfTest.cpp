#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/SelfTests.h"
#include "EditorApp/TransitionListPanel.h"
#include "EditorApp/TransitionPreview.h"
#include "EditorApp/TransitionPreviewWidget.h"

#include <QtCore/QStringList>
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
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("  %s: %s\n", ok ? "OK" : "NO", what);
        if (!ok)
            ++failures;
    };
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

    // Le due schede nella finestra.
    MainWindow window;
    QTabWidget* tabs = window.leftTabs();
    check(tabs != nullptr && tabs->count() == 2 && tabs->tabText(0) == QStringLiteral("File del progetto")
              && tabs->tabText(1) == QStringLiteral("Transizioni"),
          "il blocco in alto a sinistra ha le schede File del progetto e Transizioni");
    check(tabs != nullptr && tabs->currentIndex() == 0 && window.filesDock()->widget() == tabs,
          "all'inizio è aperta la scheda dei file, nel pannello dei file");
    check(window.transitionPanel() != nullptr && tabs != nullptr && tabs->widget(1) == window.transitionPanel()
              && window.transitionPanel()->count() == static_cast<int>(registry.size()),
          "la scheda Transizioni ha l'elenco completo");

    // Scegliendo una transizione la Sorgente ne mostra l'anteprima; il file scelto riporta il lettore.
    check(window.sourceStack() != nullptr && window.sourceStack()->currentWidget() == window.player(),
          "all'inizio la Sorgente mostra il lettore");
    window.transitionPanel()->select("scorrimento");
    check(window.sourceStack()->currentWidget() == window.transitionPreview()
              && window.transitionPreview()->transition() == core::findTransition("scorrimento"),
          "scegliendo una transizione la Sorgente ne mostra l'anteprima");
    window.sourceTabs()->setCurrentIndex(1);
    window.transitionPanel()->select("dissolvenza");
    check(window.sourceTabs()->currentIndex() == 0 && window.transitionPreview()->transition() == crossfade,
          "la scelta riporta alla scheda Sorgente e cambia l'anteprima");
    window.showSourcePlayer();
    check(window.sourceStack()->currentWidget() == window.player(), "scegliendo un file torna il lettore");

    std::printf(failures == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", failures);
    return failures == 0 ? 0 : 3;
}

}  // namespace editor::app
