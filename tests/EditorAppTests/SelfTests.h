#pragma once

// Prove da riga di comando di EditorVideoTests.exe, per la build automatica e per le verifiche a mano. La prova
// --smoke-test resta in EditorVideo.exe (vedi EditorApp/SmokeTest.h).

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdio>
#include <optional>

class QApplication;

namespace editor::app {

// Un controllo di una prova: stampa "OK" o "NO" con la descrizione e conta quelli falliti.
class SelfCheck
{
public:
    void operator()(bool ok, const char* what)
    {
        std::printf("  %s: %s\n", ok ? "OK" : "NO", what);
        if (!ok)
            ++failures_;
    }
    int failures() const { return failures_; }

private:
    int failures_ = 0;
};

// Se gli argomenti contengono una prova (--export-test, --join-test, --sequence-test, --audio-test, --project-test,
// --thumbnail-test, --codec-info), la esegue senza aprire la finestra e restituisce il codice di uscita; altrimenti
// std::nullopt. Codici: 0 tutto giusto, 1 argomenti sbagliati, 2 errore, 3 controllo non riuscito, 4 file rifiutati.
std::optional<int> runCommandLineTest(const QStringList& arguments);

// --text-test: disegna scritte con Qt e le controlla a pixel (vedi TextSelfTest.cpp).
int runTextTest(const QStringList& arguments, qsizetype at);

// --transition-test: l'elenco delle transizioni, le miniature e le due schede del blocco in alto a sinistra (vedi
// TransitionSelfTest.cpp).
int runTransitionTest(const QStringList& arguments, qsizetype at);

// --transform-test: il pannello Proprietà (campi, cursori, "Adatta"/"Riempi", ripristino), il fotogramma disegnato con scala,
// posizione, rotazione, ritaglio e opacità (a pixel) e il pannello a destra nella finestra (vedi TransformSelfTest.cpp).
int runTransformTest(const QStringList& arguments, qsizetype at);
int runLayerTest(const QStringList& arguments, qsizetype at);
int runLayerMonitorTest(const QStringList& arguments, qsizetype at);

// --image-test: le immagini fisse nel montaggio (V1 e livelli) si vedono nel monitor (vedi ImageSelfTest.cpp).
int runImageTest(const QStringList& arguments, qsizetype at);
int runManipTest(const QStringList& arguments, qsizetype at);

// --export-panel-test: la finestra "Esporta" con un riepilogo inventato: avviso di sovrascrittura, destinazioni che non
// vanno, scelte che arrivano alle opzioni (vedi ExportSelfTest.cpp).
int runExportPanelTest(const QStringList& arguments, qsizetype at);

// --group-test: la scheda Gruppi del blocco in alto a sinistra: l'elenco dei gruppi (nodi, oggetti, nodi chiusi che
// restano chiusi) e le tre schede nella finestra (vedi GroupSelfTest.cpp).
int runGroupTest(const QStringList& arguments, qsizetype at);

// Come runCommandLineTest, per le prove con la finestra vera: --dock-test, --project-export-test, --export-dialog-test
// (descritte sotto).
std::optional<int> runWindowTest(QApplication& app, const QStringList& arguments);

// --dock-test: con la finestra vera (piattaforma di Windows, non "minimal"), stacca e riaggancia la linea del tempo del
// montaggio 5 volte per ogni modo: doppio clic sul titolo di Windows (un vero messaggio WM_NCLBUTTONDBLCLK alla
// finestra staccata, senza muovere il mouse), finestra spostata e lasciata nella metà bassa, menu. Ogni volta deve
// essere agganciata in basso, a tutta larghezza e alta almeno 230 px. La finestra non prende il fuoco. 0 se va bene.
int runDockTest(QApplication& app);

// --project-export-test <progetto.evproj> <destinazione>: apre il progetto nella finestra ed esporta il montaggio come il
// pulsante Esporta (thread a parte, finestra di avanzamento), con le scelte della ricodifica di default. Scrive quanto ci
// mette; codice 0 se l'esportazione riesce, 3 se non finisce entro 5 minuti, 2 se non riesce.
int runProjectExportTest(QApplication& app, const QString& project, const QString& output);

// --export-dialog-test <progetto.evproj> <destinazione>: come l'utente, dal pulsante Esporta (finestra "Esporta" con la
// destinazione, poi il messaggio finale), con la finestra guidata da un timer e il messaggio da un altro thread. Codice 0
// se arriva in fondo e l'esportazione riesce, 3 se si blocca o non riesce.
int runExportDialogTest(QApplication& app, const QString& project, const QString& output);

}  // namespace editor::app
