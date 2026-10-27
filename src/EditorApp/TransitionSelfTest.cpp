#include "EditorApp/MainWindow.h"
#include "EditorApp/SelfTests.h"
#include "EditorApp/TransitionListPanel.h"
#include "EditorApp/TransitionPreview.h"

#include <QtCore/QStringList>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QTabWidget>

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

    std::printf(failures == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", failures);
    return failures == 0 ? 0 : 3;
}

}  // namespace editor::app
