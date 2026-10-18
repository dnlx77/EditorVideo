#include "EditorApp/MainWindow.h"

#include "EditorCore/LibraryInfo.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStatusBar>

namespace editor::app {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("EditorVideo"));
    resize(1100, 700);

    placeholder_ = new QLabel(QStringLiteral("Apri un file video da File > Apri..."), this);
    placeholder_->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder_);

    createMenus();
    statusBar()->showMessage(QStringLiteral("Pronto  ·  ") + versionSummary());
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    QAction* openAction = fileMenu->addAction(QStringLiteral("&Apri..."), this, &MainWindow::openFile);
    openAction->setShortcut(QKeySequence::Open);

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QStringLiteral("&Esci"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Aiuto"));
    helpMenu->addAction(QStringLiteral("&Informazioni su EditorVideo"), this, &MainWindow::showAbout);
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Apri file video"), QString(),
        QStringLiteral("File video (*.mp4 *.mkv *.mov *.avi *.m4v *.webm *.ts);;Tutti i file (*.*)"));
    if (path.isEmpty())
        return;

    // La lettura vera del file arriva con lo step 1: per ora mostriamo solo il percorso scelto.
    placeholder_->setText(QStringLiteral("File selezionato:\n%1\n\n(l'analisi del file arriverà con lo step 1)").arg(path));
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("Informazioni su EditorVideo"),
                       QStringLiteral("<b>EditorVideo</b><br>Taglio e unione di video senza ricodifica.<br><br>")
                           + versionSummary().toHtmlEscaped());
}

QString versionSummary()
{
    const core::FfmpegInfo ffmpeg = core::ffmpegInfo();
    return QStringLiteral("Qt %1  ·  FFmpeg %2 (libavformat %3, libavcodec %4)")
        .arg(QString::fromLatin1(qVersion()), QString::fromStdString(ffmpeg.version),
             QString::fromStdString(ffmpeg.avformat), QString::fromStdString(ffmpeg.avcodec));
}

}  // namespace editor::app
