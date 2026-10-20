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

// --smoke-test: scrive le versioni e chiude subito. Con un file (già aperto nella finestra) lo riproduce per 2 secondi,
// segna IN e OUT, costruisce un montaggio, toglie l'audio da un tratto, annulla e ripete, aspetta miniature e forme
// d'onda, salva e riapre il progetto, prova l'anteprima del montaggio ed esporta; esce con 0 se è tutto giusto, 2
// altrimenti. Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png salva anche un'immagine della finestra.
void startSmokeTest(QApplication& app, MainWindow& window, const QString& file);

}  // namespace editor::app
