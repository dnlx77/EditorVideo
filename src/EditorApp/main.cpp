#include "EditorApp/MainWindow.h"
#include "EditorApp/SelfTests.h"
#include "EditorCore/ProjectFile.h"

#include <QtWidgets/QApplication>

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[])
{
    // --smoke-test: apre la finestra, scrive le versioni su stdout e chiude subito.
    // Serve alla build automatica per verificare che l'eseguibile parta con tutte le DLL e i plugin Qt.
    // Con anche un file ("--smoke-test video.mkv") lo prova come farebbe un utente (vedi startSmokeTest).
    bool smokeTest = false;
    for (int i = 1; i < argc; ++i)
        smokeTest = smokeTest || std::strcmp(argv[i], "--smoke-test") == 0;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("EditorVideo"));

    // I percorsi si leggono da QApplication::arguments(): argv su Windows perde le lettere accentate.
    const QStringList arguments = QApplication::arguments();
    if (const auto code = editor::app::runCommandLineTest(arguments))
        return *code;

    if (const auto code = editor::app::runWindowTest(app, arguments))
        return *code;

    editor::app::MainWindow window;
    // La disposizione dei pannelli salvata l'ultima volta; le prove partono sempre da quella iniziale.
    if (!smokeTest)
        window.restoreLayout();
    window.show();

    // "EditorVideo.exe file.mp4" (o un file trascinato sull'icona) apre subito quel file; altri file dopo il primo
    // vengono aggiunti al progetto, da unire.
    QStringList files;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        if (!arguments[i].startsWith(QStringLiteral("--")))
            files.append(arguments[i]);
    }
    // Un progetto (.evproj, anche con un doppio clic se è associato a EditorVideo) si apre come progetto.
    if (files.size() == 1
        && files.front().endsWith(QString::fromLatin1(editor::core::kProjectExtension), Qt::CaseInsensitive)) {
        window.openProject(files.front());
        files.clear();
    }
    const QString file = files.isEmpty() ? QString() : files.front();
    if (!files.isEmpty()) {
        window.openPath(files.front());
        if (files.size() > 1)
            window.addFiles(files.mid(1));
    }

    if (smokeTest)
        editor::app::startSmokeTest(app, window, file);

    return app.exec();
}
