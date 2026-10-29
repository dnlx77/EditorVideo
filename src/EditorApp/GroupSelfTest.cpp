#include "EditorApp/GroupListPanel.h"
#include "EditorApp/MainWindow.h"
#include "EditorApp/SelfTests.h"

#include <QtCore/QStringList>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>

#include <cstdio>

namespace editor::app {

namespace core = editor::core;

// --group-test: l'elenco dei gruppi con righe inventate (un gruppo dell'utente, un video con il suo audio) e le tre
// schede del blocco in alto a sinistra. Codice 0 se tutto è giusto, 3 se un controllo non riesce.
int runGroupTest(const QStringList&, qsizetype)
{
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("  %s: %s\n", ok ? "OK" : "NO", what);
        if (!ok)
            ++failures;
    };
    std::printf("Prova dell'elenco dei gruppi\n");

    using Kind = core::ObjectRef::Kind;
    GroupRow user;
    user.id = 3;
    user.title = QStringLiteral("Gruppo 3");
    user.entries = {{{Kind::Piece, 1}, QStringLiteral("Video"), QStringLiteral("a.mp4"), 0.0},
                    {{Kind::TextClip, 7}, QStringLiteral("Scritta"), QStringLiteral("Titolo"), 1.5}};
    GroupRow pair;
    pair.natural = true;
    pair.title = QStringLiteral("b.mp4");
    pair.entries = {{{Kind::Piece, 2}, QStringLiteral("Video"), QStringLiteral("b.mp4"), 4.0},
                    {{Kind::PieceAudio, 2}, QStringLiteral("Audio del video"), QStringLiteral("b.mp4"), 4.0}};

    GroupListPanel panel;
    check(panel.groupCount() == 0 && panel.tree()->isHidden(), "senza gruppi l'albero è nascosto");
    panel.setGroups({user, pair});
    check(panel.groupCount() == 2, "un nodo per gruppo");
    const QTreeWidgetItem* first = panel.tree()->topLevelItem(0);
    const QTreeWidgetItem* second = panel.tree()->topLevelItem(1);
    check(first != nullptr && first->childCount() == 2 && first->text(0).startsWith(QStringLiteral("Gruppo 3")),
          "il gruppo dell'utente ha il suo nome e i suoi due oggetti");
    check(first != nullptr && first->child(1)->text(0).contains(QStringLiteral("Scritta"))
              && first->child(1)->text(0).contains(QStringLiteral("Titolo")),
          "ogni oggetto dice di che tipo è e come si chiama");
    check(second != nullptr && second->childCount() == 2 && second->font(0).italic(),
          "il video con il suo audio è un nodo a parte, in corsivo");
    check(first != nullptr && first->isExpanded(), "i nodi si aprono da soli");

    // Un nodo chiuso dall'utente resta chiuso quando l'elenco si rifà; lo stesso elenco non tocca la vista.
    panel.tree()->collapseItem(panel.tree()->topLevelItem(0));
    GroupRow bigger = user;
    bigger.entries.push_back({{Kind::AudioClip, 9}, QStringLiteral("Audio"), QStringLiteral("musica.mp3"), 2.0});
    panel.setGroups({bigger, pair});
    check(panel.tree()->topLevelItem(0)->childCount() == 3 && !panel.tree()->topLevelItem(0)->isExpanded(),
          "un oggetto in più compare e il nodo chiuso resta chiuso");
    const QTreeWidgetItem* before = panel.tree()->topLevelItem(0);
    panel.setGroups({bigger, pair});
    check(panel.tree()->topLevelItem(0) == before, "se non è cambiato niente la vista non si rifà");

    // Scegliere nell'elenco: un gruppo dà tutti i suoi oggetti, un oggetto solo lui; il tempo è il più presto.
    std::vector<core::ObjectRef> chosen;
    double chosenStart = -1.0;
    int chosenCalls = 0;
    panel.onChosen = [&](const std::vector<core::ObjectRef>& refs, double start) {
        chosen = refs;
        chosenStart = start;
        ++chosenCalls;
    };
    panel.tree()->setCurrentItem(panel.tree()->topLevelItem(1));
    check(chosen.size() == 2 && chosen[0] == core::ObjectRef{Kind::Piece, 2} && chosen[1] == core::ObjectRef{Kind::PieceAudio, 2}
              && chosenStart == 4.0,
          "scegliere un gruppo dà tutti i suoi oggetti e l'inizio");
    panel.tree()->setCurrentItem(panel.tree()->topLevelItem(0)->child(2));
    check(chosen.size() == 1 && chosen[0] == core::ObjectRef{Kind::AudioClip, 9} && chosenStart == 2.0,
          "scegliere un oggetto dà solo lui e il suo inizio");
    panel.tree()->setCurrentItem(panel.tree()->topLevelItem(0));
    check(chosen.size() == 3 && chosenStart == 0.0, "scegliendo il gruppo dell'utente: i suoi tre oggetti, dal più presto");

    // Ciò che è scelto sulla linea del tempo si evidenzia, senza richiamare onChosen.
    const int calls = chosenCalls;
    panel.showSelection({{Kind::Piece, 2}, {Kind::PieceAudio, 2}});
    check(panel.tree()->currentItem() == panel.tree()->topLevelItem(1), "tutti gli oggetti di un gruppo: il suo nodo");
    panel.showSelection({{Kind::TextClip, 7}});
    check(panel.tree()->currentItem() == panel.tree()->topLevelItem(0)->child(1), "un oggetto solo: la sua voce");
    panel.showSelection({{Kind::Piece, 1}, {Kind::Piece, 2}});
    check(panel.tree()->currentItem() == nullptr, "oggetti di gruppi diversi: niente di evidenziato");
    panel.showSelection({});
    check(panel.tree()->currentItem() == nullptr && chosenCalls == calls, "nessuna selezione: niente, e onChosen non è chiamata");

    panel.setGroups({});
    check(panel.groupCount() == 0 && panel.tree()->isHidden(), "senza più gruppi l'albero si nasconde");

    // Le tre schede nella finestra.
    MainWindow window;
    QTabWidget* tabs = window.leftTabs();
    check(tabs != nullptr && tabs->count() == 3 && tabs->tabText(0) == QStringLiteral("File del progetto")
              && tabs->tabText(1) == QStringLiteral("Transizioni") && tabs->tabText(2) == QStringLiteral("Gruppi"),
          "il blocco in alto a sinistra ha le schede File del progetto, Transizioni e Gruppi");
    check(window.groupPanel() != nullptr && tabs != nullptr && tabs->widget(2) == window.groupPanel()
              && window.groupPanel()->groupCount() == 0,
          "la scheda Gruppi c'è ed è vuota in un progetto vuoto");

    std::printf(failures == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", failures);
    return failures == 0 ? 0 : 3;
}

}  // namespace editor::app
