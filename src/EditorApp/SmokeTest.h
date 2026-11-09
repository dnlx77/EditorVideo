#pragma once

// La prova --smoke-test di EditorVideo.exe: verifica l'eseguibile vero, con le sue DLL e i plugin di Qt. Le altre prove
// sono in EditorVideoTests.exe (tests/EditorAppTests).

#include <QtCore/QString>

class QApplication;

namespace editor::app {

class MainWindow;

// --smoke-test: scrive le versioni e chiude subito. Con un file (già aperto nella finestra) lo riproduce per 2 secondi,
// segna IN e OUT, costruisce un montaggio, toglie l'audio da un tratto, annulla e ripete, aspetta miniature e forme
// d'onda, salva e riapre il progetto, prova l'anteprima del montaggio ed esporta; esce con 0 se è tutto giusto, 2
// altrimenti. Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png salva anche un'immagine della finestra.
void startSmokeTest(QApplication& app, MainWindow& window, const QString& file);

}  // namespace editor::app
