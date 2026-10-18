#include "EditorApp/MainWindow.h"

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[])
{
    // --smoke-test: apre la finestra, scrive le versioni su stdout e chiude subito.
    // Serve alla build automatica per verificare che l'eseguibile parta con tutte le DLL e i plugin Qt.
    bool smokeTest = false;
    for (int i = 1; i < argc; ++i)
        smokeTest = smokeTest || std::strcmp(argv[i], "--smoke-test") == 0;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("EditorVideo"));

    editor::app::MainWindow window;
    window.show();

    if (smokeTest) {
        std::printf("Piattaforma Qt: %s\n%s\n", qPrintable(QGuiApplication::platformName()),
                    qPrintable(editor::app::versionSummary()));
        std::fflush(stdout);
        QTimer::singleShot(0, &app, &QApplication::quit);
    }

    return app.exec();
}
