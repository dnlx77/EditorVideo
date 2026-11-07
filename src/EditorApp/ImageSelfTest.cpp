#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SelfTests.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/StillImage.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QImage>

#include <cstdio>

namespace editor::app {

namespace {

PlayerWidget* findPlayer(ProgramMonitor& monitor)
{
    for (QWidget* w : monitor.findChildren<QWidget*>()) {  // PlayerWidget non ha Q_OBJECT: findChild non lo accetta
        if (auto* player = dynamic_cast<PlayerWidget*>(w))
            return player;
    }
    return nullptr;
}

// Il colore al centro dell'immagine del monitor, dopo aver aspettato (al massimo ~2 s) che somigli a `expected`.
QColor centerColor(ProgramMonitor& monitor, PlayerWidget& player, const QColor& expected)
{
    QColor color;
    for (int i = 0; i < 100; ++i) {
        QCoreApplication::processEvents();
        const QRectF canvas = player.imageRect();
        if (!canvas.isEmpty()) {
            const QImage shot = player.grab().toImage();
            const QPoint p = canvas.center().toPoint();
            if (shot.rect().contains(p)) {
                color = shot.pixelColor(p);
                if (std::abs(color.red() - expected.red()) < 40 && std::abs(color.green() - expected.green()) < 40 &&
                    std::abs(color.blue() - expected.blue()) < 40)
                    return color;
            }
        }
        QThread::msleep(20);
    }
    (void)monitor;
    return color;
}

}  // namespace

// --image-test: le immagini nel montaggio. Si genera un'immagine rossa e una verde (PNG), la rossa su V1 e la verde su V2 al
// centro del fotogramma; il monitor del Montaggio le mostra (da sole e sovrapposte) e senza immagini sotto V1 è nera.
// Codice 0 se tutto è giusto, 3 se un controllo non riesce.
int runImageTest(const QStringList&, qsizetype)
{
    namespace core = editor::core;
    SelfCheck check;
    std::printf("Prova delle immagini nel montaggio\n");

    const QString dir = QDir::tempPath() + QStringLiteral("/EditorVideoImageTest");
    QDir().mkpath(dir);
    QImage red(160, 90, QImage::Format_RGB32);
    red.fill(QColor(220, 20, 20));
    QImage green(80, 80, QImage::Format_RGB32);
    green.fill(QColor(20, 220, 20));
    const QString redPath = dir + QStringLiteral("/rossa.png");
    const QString greenPath = dir + QStringLiteral("/verde.png");
    check(red.save(redPath) && green.save(greenPath), "le immagini di prova si scrivono");

    core::Project project;
    project.setFormat(core::defaultProjectFormat());
    const int redClip = project.add(redPath.toStdString());
    const int greenClip = project.add(greenPath.toStdString());
    check(project.find(redClip)->still && project.find(greenClip)->still, "le immagini sono riconosciute dal progetto");
    for (const int id : {redClip, greenClip}) {
        const core::MediaInfo info = core::probeFile(project.find(id)->path);
        project.setAnalysis(id, info.keyframeTimes, info.durationSeconds);
    }
    check(project.find(redClip)->duration == core::kStillDuration, "la durata nel file è quella delle immagini");
    check(project.sequence().insert(-1, redClip, 0.0, 3.0) != 0, "l'immagine rossa entra in V1 per 3 secondi");
    const int layer = project.textTracks().addLayer();
    core::LayerPiece piece;
    piece.clipId = greenClip;
    piece.start = 0.0;
    piece.end = 1.0;
    piece.position = 1.0;
    piece.transform.scaleX = 0.5;
    piece.transform.scaleY = 0.5;
    check(project.textTracks().addPiece(layer, piece) != 0, "l'immagine verde entra su V2");

    ProgramMonitor monitor(project);
    monitor.resize(900, 600);
    monitor.show();
    monitor.sequenceChanged();
    QCoreApplication::processEvents();
    PlayerWidget* player = findPlayer(monitor);
    check(player != nullptr, "il monitor ha il lettore");
    if (player == nullptr)
        return 3;

    monitor.seekTo(0.5);
    const QColor first = centerColor(monitor, *player, QColor(220, 20, 20));
    check(first.red() > 150 && first.green() < 80, "a 0,5 s il monitor mostra l'immagine rossa di V1");

    monitor.seekTo(2.0);
    const QColor later = centerColor(monitor, *player, QColor(220, 20, 20));
    check(later.red() > 150 && later.green() < 80, "a 2 s (dopo 1,5 s di immagine) è ancora rossa: lo stesso fotogramma a ogni tempo");

    monitor.seekTo(1.5);
    QCoreApplication::processEvents();
    check(monitor.layerFramesShown() == 1, "a 1,5 s si vede il pezzo-immagine di V2");
    const QColor middle = centerColor(monitor, *player, QColor(20, 220, 20));
    check(middle.green() > 150 && middle.red() < 80, "al centro, sopra la rossa, c'è la verde di V2");

    monitor.seekTo(2.5);
    QCoreApplication::processEvents();
    check(monitor.layerFramesShown() == 0, "dopo la fine del pezzo di V2 non si vede più");

    // In riproduzione sull'immagine di V1 (da prima di V2): il tempo avanza e il fotogramma resta quello.
    monitor.seekTo(0.1);
    monitor.setPlaying(true);
    for (int i = 0; i < 150 && monitor.position() < 0.8; ++i) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    const double reached = monitor.position();
    const QColor playing = centerColor(monitor, *player, QColor(220, 20, 20));
    monitor.setPlaying(false);
    check(reached >= 0.8, "in riproduzione il tempo avanza sull'immagine");
    check(playing.red() > 150 && playing.green() < 80, "durante la riproduzione l'immagine di V1 resta rossa");

    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
