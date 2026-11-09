#include "SelfTests.h"

#include "EditorApp/MainWindow.h"
#include "EditorApp/QtImageLoader.h"

#include <QtWidgets/QApplication>

#include <cstdio>

// EditorVideoTests.exe: le prove della finestra e dell'esportazione da riga di comando (vedi SelfTests.h), con lo stesso
// codice di EditorVideo.exe (libreria EditorAppLib) ma fuori dal programma che si usa.
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("EditorVideo"));
    editor::app::installQtImageLoader();
    // Le prove non aprono finestre di conferma.
    editor::app::MainWindow::confirmRemovals = false;

    // I percorsi si leggono da QApplication::arguments(): argv su Windows perde le lettere accentate.
    const QStringList arguments = QApplication::arguments();
    if (const auto code = editor::app::runCommandLineTest(arguments))
        return *code;
    if (const auto code = editor::app::runWindowTest(app, arguments))
        return *code;
    std::printf("Uso: EditorVideoTests.exe --<nome>-test [argomenti] (vedi README, sezione Test).\n");
    return 1;
}
