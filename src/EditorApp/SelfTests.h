#pragma once

// Prove da riga di comando, per la build automatica e per le verifiche a mano.

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

class QApplication;

namespace editor::app {

class MainWindow;

// Se gli argomenti contengono una prova (--export-test, --join-test, --sequence-test, --audio-test, --project-test,
// --thumbnail-test, --codec-info), la esegue senza aprire la finestra e restituisce il codice di uscita; altrimenti
// std::nullopt. Codici: 0 tutto giusto, 1 argomenti sbagliati, 2 errore, 3 controllo non riuscito, 4 file rifiutati.
std::optional<int> runCommandLineTest(const QStringList& arguments);

// --text-test: disegna scritte con Qt e le controlla a pixel (vedi TextSelfTest.cpp).
int runTextTest(const QStringList& arguments, qsizetype at);

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

// --export-dialog-test <progetto.evproj> <destinazione>: come l'utente, dal pulsante Esporta (finestra del riepilogo,
// finestra di Windows per salvare, messaggio finale), con le finestre guidate da un altro thread. Codice 0 se arriva in
// fondo e l'esportazione riesce, 3 se si blocca o non riesce.
int runExportDialogTest(QApplication& app, const QString& project, const QString& output);

// --smoke-test: scrive le versioni e chiude subito. Con un file (già aperto nella finestra) lo riproduce per 2 secondi,
// segna IN e OUT, costruisce un montaggio, toglie l'audio da un tratto, annulla e ripete, aspetta miniature e forme
// d'onda, salva e riapre il progetto, prova l'anteprima del montaggio ed esporta; esce con 0 se è tutto giusto, 2
// altrimenti. Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png salva anche un'immagine della finestra.
void startSmokeTest(QApplication& app, MainWindow& window, const QString& file);

}  // namespace editor::app
