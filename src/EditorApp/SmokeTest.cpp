#include "EditorApp/SmokeTest.h"

#include "EditorApp/GroupListPanel.h"
#include "EditorApp/ExportDialog.h"
#include "EditorApp/MainWindow.h"
#include "EditorApp/MixerPanel.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/PropertiesPanel.h"
#include "EditorApp/TestAccess.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorApp/TransitionMime.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TimelineWidget.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/FileDrop.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/Proxy.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Thumbnails.h"
#include "EditorCore/TimeUtil.h"
#include "EditorCore/VideoDecoder.h"

#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>

#include <algorithm>

#include <windows.h>
#include "EditorCore/Encoder.h"
#include <atomic>
#include <thread>
#include <QtWidgets/QFileDialog>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>


namespace editor::app {

namespace {

constexpr int kSmokePlayMilliseconds = 2000;

namespace core = editor::core;

// Quello che serve a tutti i passi della prova. Ogni passo (check...) scrive cosa ha visto e restituisce false se
// qualcosa non va; i passi girano tutti anche dopo un errore, e il codice di uscita li riassume.
struct SmokeRun
{
    SmokeRun(QApplication& application, MainWindow& mainWindow, const QString& path)
        : app(application), window(&mainWindow), player(mainWindow.player()), file(path)
    {
    }

    QApplication& app;
    MainWindow* window;
    PlayerWidget* player;
    QString file;
    QElapsedTimer playStart;  // il tempo vero dall'inizio della riproduzione (su un PC lento il controllo arriva tardi)
    double pausedAt = 0.0;    // dove la Sorgente si è fermata dopo i primi 2 s (checkPlayback)
    QElapsedTimer analysis;   // dall'attesa dell'analisi dei file (buildSequence)
};

// Fine della prova: codice di uscita 0 se è andato tutto bene, altrimenti 2. Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png
// salva anche un'immagine della finestra.
void finishSmokeTest(SmokeRun& run, bool success)
{
    const QString screenshot = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QCoreApplication::processEvents();  // applica i cambi di layout ancora in sospeso
        run.window->grab().save(screenshot);
    }
    run.app.exit(success ? 0 : 2);
}

// Riproduzione della Sorgente dopo 2 s: deve essere partita e avanzare col tempo giusto (±25%). Poi pausa e un tratto
// IN-OUT di 5 fotogrammi.
bool checkPlayback(SmokeRun& run)
{
    PlayerWidget* player = run.player;
    const double position = player->currentPosition();
    const double expected = run.playStart.isValid() ? run.playStart.elapsed() / 1000.0 : kSmokePlayMilliseconds / 1000.0;
    std::printf("Riproduzione: %s, posizione %.3f s dopo %.1f s (passati davvero %.3f s), %s\n",
                player->isPlaying() ? "in corso" : "ferma", position, kSmokePlayMilliseconds / 1000.0, expected,
                qPrintable(TestAccess::statusText(*player)));
    std::fflush(stdout);
    // Codice di uscita 2 se la riproduzione non è partita o non avanza col tempo giusto (±25%).
    bool ok = player->isPlaying() && std::abs(position - expected) < expected * 0.25;
    if (!ok)
        std::printf("  NON riuscito: la riproduzione %s\n",
                    player->isPlaying() ? "non avanza col tempo giusto" : "non è partita o si è fermata");

    // Segmento: pausa, IN qui, 5 fotogrammi avanti, OUT. La posizione si rilegge dopo la pausa: fra la lettura
    // di sopra e questo punto la riproduzione va ancora avanti (con un file solo audio anche più di 0,1 s).
    if (player->isPlaying())
        player->togglePlayPause();
    run.pausedAt = player->currentPosition();
    player->markIn();
    for (int i = 0; i < 5; ++i)
        player->stepFrame(+1);
    player->markOut();
    const core::CutSegment& segment = player->segment();
    const auto& keyframes = segment.keyframes();
    const bool inOnKeyframe = keyframes.empty()
                           || std::any_of(keyframes.begin(), keyframes.end(),
                                          [&](double k) { return std::abs(k - segment.in()) < 1e-6; });
    std::printf("Segmento: IN %.3f s, OUT %.3f s, durata %.3f s%s\n", segment.in(), segment.out(),
                segment.length(), inOnKeyframe ? "" : " (IN fra due fotogrammi chiave: taglio preciso)");
    std::fflush(stdout);
    // IN resta dove lo si mette (taglio preciso): non oltre la posizione della pausa.
    const bool segmentOk = segment.hasIn() && segment.hasOut() && segment.in() <= run.pausedAt + 0.1
                        && segment.length() > 0;
    if (!segmentOk)
        std::printf("  NON riuscito: segmento IN-OUT non valido\n");
    ok = ok && segmentOk;
    return ok;
}

// Un file solo audio non entra mai nella traccia video: con A va su una traccia audio. (Il resto della prova, con pezzi,
// giunzioni ed esportazione, ha bisogno di un video.)
bool checkAudioOnlyFile(SmokeRun& run)
{
    MainWindow* window = run.window;
    window->addToSequence();
    const core::AudioTracks& audioTracks = window->project().audioTracks();
    const bool onAudioTrack = window->project().sequence().empty() && audioTracks.tracks().size() == 1
                           && audioTracks.tracks()[0].clips.size() == 1;
    std::printf("Solo audio: con A il file va su una traccia audio e non su quella video %s\n", onAudioTrack ? "sì" : "NO");
    std::fflush(stdout);
    return onAudioTrack;
}

// Montaggio: il pezzo appena segnato, poi uno all'inizio dello stesso file (A), poi gli altri file interi.
bool buildSequence(SmokeRun& run)
{
    MainWindow* window = run.window;
    PlayerWidget* player = run.player;
    window->addToSequence();
    player->seekTo(0.0);
    player->markIn();
    for (int i = 0; i < 3; ++i)
        player->stepFrame(+1);
    player->markOut();
    window->addToSequence();

    // Con più file si aspetta che siano stati analizzati tutti (servono durata e fotogrammi chiave).
    run.analysis.start();
    while (!window->project().allAnalyzed() && run.analysis.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const int firstClip = window->currentClipId();
    for (const core::Clip& clip : window->project().clips()) {
        if (clip.id != firstClip) {
            window->selectClip(clip.id);
            window->addToSequence();  // senza IN e OUT: il file intero
        }
    }
    const core::Sequence& sequence = window->project().sequence();
    const std::size_t expectedPieces = 2 + window->project().clips().size() - 1;
    std::printf("Montaggio: %d pezzi, durata %.3f s\n", static_cast<int>(sequence.fragments().size()),
                sequence.totalLength());
    std::fflush(stdout);
    return sequence.fragments().size() == expectedPieces;
}

// Pulsanti dei player: velocità 2x, stop (torna al punto di partenza), all'indietro e pausa (la spola
// finisce). Il tempo si misura davvero, con margini larghi: su un PC lento i tick arrivano in ritardo.
bool checkPlayerButtons(SmokeRun& run)
{
    PlayerWidget* player = run.player;
    const double pausedAt = run.pausedAt;
    const auto runFor = [](int milliseconds) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < milliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return t.elapsed() / 1000.0;
    };
    player->seekTo(0.0);
    player->setBaseSpeed(2.0);
    player->play();
    const double fastElapsed = runFor(600);
    const double fast = player->currentPosition();
    player->togglePlayPause();
    player->setBaseSpeed(1.0);
    const bool fastOk = std::abs(fast - 2.0 * fastElapsed) < 2.0 * fastElapsed * 0.35;
    player->stop();
    const bool stopOk = player->currentPosition() < 0.05 && !player->isPlaying();
    bool reverseOk = true;
    double reverseFrom = pausedAt;
    double reverseTo = pausedAt;
    if (pausedAt > 0.6) {
        player->seekTo(pausedAt);
        player->shuttle(-1);
        runFor(300);
        reverseTo = player->currentPosition();
        reverseOk = player->speed() < 0 && reverseTo < reverseFrom - 0.1;
        player->togglePlayPause();
        reverseOk = reverseOk && player->speed() == 1.0 && !player->isPlaying();
    }
    std::printf("Comandi: 2x %.2f s in %.2f s %s, stop %s, all'indietro da %.2f a %.2f s %s\n", fast,
                fastElapsed, fastOk ? "ok" : "NO", stopOk ? "ok" : "NO", reverseFrom, reverseTo,
                reverseOk ? "ok" : "NO");
    std::fflush(stdout);
    return fastOk && stopOk && reverseOk;
}

// Proxy: anteprima a 1/2, si aspetta il proxy del file (creato in sottofondo con l'encoder della scheda
// video o di Windows) e la Sorgente passa a leggerlo restando allo stesso punto; poi la qualità torna com'era
// e il proxy fatto per la prova si cancella.
bool checkProxy(SmokeRun& run)
{
    MainWindow* window = run.window;
    PlayerWidget* player = run.player;
    const QString& file = run.file;
    const double pausedAt = run.pausedAt;
    // Serve un video (la riga "Decodifica" della Sorgente) e un encoder H.264 per crearne il proxy.
    if (!TestAccess::statusText(*player).contains(QStringLiteral("Decodifica")) || !core::preferredVideoEncoder(core::VideoCodec::H264))
        return true;
    const int quality = TestAccess::previewQuality(*window, TestAccess::currentFile(*window));
    const double at = std::min(pausedAt, 0.5);
    player->seekTo(at);
    QElapsedTimer wait;
    wait.start();
    window->setPreviewQuality(TestAccess::currentFile(*window), 2);
    while (!TestAccess::statusText(*player).contains(QStringLiteral("proxy")) && wait.elapsed() < 60000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const bool usesProxy = TestAccess::statusText(*player).contains(QStringLiteral("proxy"));
    const double playedAt = player->currentPosition();
    const bool positionOk = std::abs(playedAt - at) < 0.05;
    std::printf("Proxy 1/2: %s in %.1f s, posizione %.3f s (era %.3f) %s\n",
                usesProxy ? "in uso" : "NON in uso", wait.elapsed() / 1000.0, playedAt, at, positionOk ? "ok" : "NO");
    std::fflush(stdout);
    window->setPreviewQuality(TestAccess::currentFile(*window), quality);
    if (quality != 2) {
        const std::string name = core::proxyFileName(QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()).toStdString(), 2);
        QFile::remove(QDir(window->proxyFolder()).filePath(QString::fromStdString(name)));
    }
    return usesProxy && positionOk;
}

// Annulla e Ripeti: il silenzio appena messo sparisce e torna; l'asterisco segue.
bool checkUndoRedo(SmokeRun& run)
{
    MainWindow* window = run.window;
    const core::Sequence& sequence = window->project().sequence();
    const QString label = window->undoText();
    const bool undone = window->undo() && sequence.mutedRanges().empty();
    const bool redone = window->redo() && sequence.mutedRanges().size() == 1;
    std::printf("Annulla / Ripeti: \"%s\" %s, ripetuto %s\n", qPrintable(label.mid(1)),
                undone ? "annullato" : "NON annullato", redone ? "sì" : "NO");
    std::fflush(stdout);
    return undone && redone && label.contains(QStringLiteral("Togli audio"));
}

// Miniature della timeline: si disegna la finestra (che le chiede) e si aspetta che siano pronte.
bool checkThumbnails(SmokeRun& run)
{
    MainWindow* window = run.window;
    window->grab();
    QElapsedTimer thumbs;
    thumbs.start();
    while (TestAccess::pendingThumbnails(*window) > 0 && thumbs.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const bool hasVideo = TestAccess::statusText(*window->player()).contains(QStringLiteral("Decodifica"));
    std::printf("Miniature: %d pronte, %d in attesa\n", TestAccess::readyThumbnails(*window), TestAccess::pendingThumbnails(*window));
    std::fflush(stdout);
    return TestAccess::pendingThumbnails(*window) == 0 && (!hasVideo || TestAccess::readyThumbnails(*window) > 0);
}

// Zoom delle due linee del tempo: al massimo (12 pixel per fotogramma), disegnate, scorse fino in fondo e
// di nuovo "Adatta". Con lo zoom al massimo si chiedono solo le miniature visibili.
bool checkZoom(SmokeRun& run)
{
    MainWindow* window = run.window;
    bool zoomOk = true;
    for (ZoomableTimeline* timeline :
         {static_cast<ZoomableTimeline*>(TestAccess::sequenceTimeline(*window)), static_cast<ZoomableTimeline*>(window->player()->timeline())}) {
        for (int i = 0; i < 80; ++i)
            timeline->zoomIn();
        const auto& zoom = timeline->zoom();
        const bool atMax = zoom.duration() <= 0
                        || std::abs(zoom.pixelsPerSecond() - zoom.maxPixelsPerSecond()) < 1e-6;
        timeline->grab();
        QWheelEvent wheel(QPointF(10, 10), timeline->mapToGlobal(QPointF(10, 10)), QPoint(), QPoint(0, -120 * 50),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(timeline, &wheel);
        timeline->grab();
        // Oltre "Adatta": tutto in un decimo della larghezza, senza barra di scorrimento.
        for (int i = 0; i < 80; ++i)
            timeline->zoomOut();
        const bool atMin = zoom.duration() <= 0
                        || (std::abs(zoom.contentWidth() - zoom.width() * 0.1) < 1.0 && !zoom.canScroll());
        timeline->grab();
        timeline->fitAll();
        timeline->grab();
        zoomOk = zoomOk && atMax && atMin && timeline->zoom().isFit();
    }
    QCoreApplication::processEvents();
    std::printf("Zoom delle linee del tempo: massimo, minimo e Adatta %s, miniature in attesa %d\n", zoomOk ? "OK" : "NON riusciti",
                TestAccess::pendingThumbnails(*window));
    std::fflush(stdout);
    return zoomOk && TestAccess::pendingThumbnails(*window) < 200;
    QElapsedTimer thumbs;
    thumbs.start();
    while (TestAccess::pendingThumbnails(*window) > 0 && thumbs.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

// Pannelli: la Sorgente staccata come finestra durante la riproduzione (il video deve continuare ad andare
// e a mostrarsi), riagganciata, a schermo intero e ritorno; disposizione salvata e ripristinata.
bool checkPanels(SmokeRun& run)
{
    MainWindow* window = run.window;
    PlayerWidget* player = run.player;
    const auto advances = [&](const char* when) {
        const double from = player->currentPosition();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 700)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const double moved = player->currentPosition() - from;
        std::printf("  %s: la riproduzione avanza di %.2f s in 0,7 s (%s)\n", when, moved,
                    qPrintable(TestAccess::statusText(*player)));
        return moved > 0.3;
    };
    QDockWidget* source = TestAccess::sourceDock(*window);
    player->seekTo(0.0);
    player->play();
    bool docksOk = advances("agganciata");
    source->setFloating(true);
    docksOk = advances("staccata") && source->isFloating() && docksOk;
    source->setFloating(false);
    docksOk = advances("riagganciata") && !source->isFloating() && docksOk;
    window->toggleFullScreen(source);
    const bool full = TestAccess::isPanelFullScreen(*window) && source->isFloating();
    docksOk = advances("a schermo intero") && full && docksOk;
    window->toggleFullScreen(source);
    docksOk = docksOk && !TestAccess::isPanelFullScreen(*window) && !source->isFloating();
    player->pause();

    // Disposizione: salvata, cambiata (pannello spostato e uno nascosto), ripristinata.
    const QByteArray state = window->saveState();
    const Qt::DockWidgetArea area = window->dockWidgetArea(TestAccess::filesDock(*window));
    window->addDockWidget(Qt::RightDockWidgetArea, TestAccess::filesDock(*window));
    TestAccess::timelineDock(*window)->hide();
    const bool changed = window->dockWidgetArea(TestAccess::filesDock(*window)) == Qt::RightDockWidgetArea;
    const bool restored = window->restoreState(state) && window->dockWidgetArea(TestAccess::filesDock(*window)) == area
                       && TestAccess::timelineDock(*window)->isVisible();
    TestAccess::timelineDock(*window)->hide();
    window->resetLayout();
    const bool reset = TestAccess::timelineDock(*window)->isVisible() && !TestAccess::sourceDock(*window)->isFloating();

    // La linea del tempo staccata e riagganciata torna sotto a tutto, larga quanto la finestra.
    QDockWidget* timelineDock = TestAccess::timelineDock(*window);
    timelineDock->setFloating(true);
    QCoreApplication::processEvents();
    timelineDock->setFloating(false);
    window->addDockWidget(Qt::BottomDockWidgetArea, timelineDock);
    QCoreApplication::processEvents();
    const bool fullWidth = window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea
                        && timelineDock->width() >= window->width() - 30;

    // Staccata e lasciata dall'utente: nella metà alta della finestra resta staccata, nella metà bassa
    // (a pulsante rilasciato) si riaggancia sotto a tutto; il comando del menu la riaggancia sempre.
    timelineDock->setFloating(true);
    QCoreApplication::processEvents();
    const QRect frame = window->frameGeometry();
    const bool highStays = !window->redockTimelineIfDroppedLow(frame.center() - QPoint(0, frame.height() / 4), false)
                        && timelineDock->isFloating();
    const bool heldStays = !window->redockTimelineIfDroppedLow(frame.center() + QPoint(0, frame.height() / 4), true)
                        && timelineDock->isFloating();
    const bool lowDocks = window->redockTimelineIfDroppedLow(frame.center() + QPoint(0, frame.height() / 4), false)
                       && !timelineDock->isFloating()
                       && window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea;
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    const bool lowFull = timelineDock->width() >= window->width() - 30 && timelineDock->height() >= 225;
    const int lowHeight = timelineDock->height();
    timelineDock->setFloating(true);
    window->redockTimeline();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    const bool menuDocks = !timelineDock->isFloating() && timelineDock->width() >= window->width() - 30
                        && timelineDock->height() >= 225;
    const int menuHeight = timelineDock->height();
    // Più alta prima di staccarla: riagganciata riprende quell'altezza.
    window->resizeDocks({timelineDock}, {400}, Qt::Vertical);
    QCoreApplication::processEvents();
    const int before = timelineDock->height();
    timelineDock->setFloating(true);
    QCoreApplication::processEvents();
    timelineDock->setFloating(false);  // come il doppio clic sul titolo
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    const bool keptHeight = !timelineDock->isFloating() && std::abs(timelineDock->height() - before) <= 4;
    std::printf("  altezza della linea del tempo riagganciata: %d px (in basso), %d px dal menu, %d px dopo il doppio "
                "clic (prima %d)\n",
                lowHeight, menuHeight, timelineDock->height(), before);
    std::printf("  linea del tempo lasciata in alto: %s, trascinata: %s, lasciata in basso: %s, dal menu: %s\n",
                highStays ? "resta staccata" : "AGGANCIATA", heldStays ? "resta staccata" : "AGGANCIATA",
                lowDocks && lowFull ? "riagganciata a tutta larghezza" : "NON riagganciata",
                menuDocks ? "riagganciata" : "NON riagganciata");
    // Cicli ripetuti, come farebbe l'utente: staccata, trascinata (pulsante premuto, finestra spostata) e lasciata
    // nella metà bassa; poi staccata e riagganciata con il doppio clic; poi dal menu. Ogni volta in basso, a
    // tutta larghezza e alta almeno 230 px.
    int cycles = 0;
    for (int i = 0; i < 5; ++i) {
        const auto docked = [&] {
            QElapsedTimer wait;  // i riagganci partono da timer: si aspetta davvero
            wait.start();
            while (wait.elapsed() < 250)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return !timelineDock->isFloating() && window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea
                && timelineDock->width() >= window->width() - 30 && timelineDock->height() >= 225;
        };
        timelineDock->setFloating(true);
        QCoreApplication::processEvents();
        window->timelinePollStep(true, frame.center() - QPoint(0, frame.height() / 4));  // premuto sul titolo
        timelineDock->move(timelineDock->pos() + QPoint(30, 120));                    // trascinata
        window->timelinePollStep(true, frame.center() + QPoint(0, frame.height() / 4));
        window->timelinePollStep(false, frame.center() + QPoint(0, frame.height() / 4));  // lasciata in basso
        const bool dragged = docked();
        timelineDock->setFloating(true);
        QCoreApplication::processEvents();
        timelineDock->setFloating(false);  // come il doppio clic sul titolo
        const bool doubleClick = docked();
        timelineDock->setFloating(true);
        QCoreApplication::processEvents();
        window->redockTimeline();
        const bool menu = docked();
        // Staccata, spostata e lasciata in alto: resta staccata.
        timelineDock->setFloating(true);
        QCoreApplication::processEvents();
        window->timelinePollStep(true, frame.center());
        timelineDock->move(timelineDock->pos() + QPoint(10, -60));
        window->timelinePollStep(false, frame.center() - QPoint(0, frame.height() / 4));
        docked();
        const bool stays = timelineDock->isFloating();
        window->redockTimeline();
        docked();
        cycles += dragged && doubleClick && menu && stays ? 1 : 0;
    }
    std::printf("  cicli stacca/riaggancia riusciti: %d su 5\n", cycles);

    // Titolo evidenziato con il mouse sopra (e non sul contenuto).
    QDockWidget* titled = TestAccess::sourceDock(*window);
    const auto hover = [&](QPointF at) {
        QHoverEvent e(QEvent::HoverMove, at, titled->mapToGlobal(at), QPointF(-1, -1));
        QCoreApplication::sendEvent(titled, &e);
    };
    hover(QPointF(titled->width() / 2.0, 6));
    const bool titleLit = window->isTitleHighlighted(titled) && titled->cursor().shape() == Qt::OpenHandCursor;
    hover(QPointF(titled->width() / 2.0, titled->height() / 2.0));
    const bool contentNotLit = !window->isTitleHighlighted(titled);
    std::printf("  titolo dei pannelli: evidenziato %s, sul contenuto no %s\n", titleLit ? "OK" : "NO",
                contentNotLit ? "OK" : "NO");

    const bool redockOk = highStays && heldStays && lowDocks && lowFull && menuDocks && keptHeight && cycles == 5
                       && titleLit && contentNotLit;
    window->resetLayout();
    QCoreApplication::processEvents();
    window->resetLayout();
    QCoreApplication::processEvents();

    // Incrocio fra il separatore verticale (file | Sorgente) e quello orizzontale (sopra | linea del tempo):
    // trascinandolo cambiano insieme la larghezza dei file e l'altezza della linea del tempo.
    QDockWidget* files = TestAccess::filesDock(*window);
    const QRect fg = files->geometry();
    const QRect sg = TestAccess::sourceDock(*window)->geometry();
    const QRect tg = timelineDock->geometry();
    const QPoint corner((fg.right() + sg.left()) / 2 + 1, (fg.bottom() + tg.top()) / 2 + 1);
    const int filesWidth = files->width();
    const int timelineHeight = timelineDock->height();
    const auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(at), window->mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &e);
    };
    mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, corner + QPoint(40, -30), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, corner + QPoint(40, -30), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    const int widthChange = files->width() - filesWidth;
    const int heightChange = timelineDock->height() - timelineHeight;
    const bool cornerOk = std::abs(widthChange - 40) <= 4 && std::abs(heightChange - 30) <= 4;
    std::printf("  incrocio dei separatori in (%d, %d): file %+d px, linea del tempo %+d px (attesi +40 e +30)\n",
                corner.x(), corner.y(), widthChange, heightChange);
    window->resetLayout();

    // Pannelli piccoli: la misura minima di ognuno e, con EDITORVIDEO_SMOKE_SCREENSHOT, un'immagine di ognuno
    // staccato a 170x120.
    bool smallOk = true;
    const QString shots = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
    for (QDockWidget* dock : {TestAccess::filesDock(*window), TestAccess::sourceDock(*window), TestAccess::programDock(*window), timelineDock}) {
        const QSize minimum = dock->minimumSizeHint().expandedTo(dock->minimumSize());
        std::printf("  pannello %s: misura minima %dx%d\n", qPrintable(dock->windowTitle()), minimum.width(),
                    minimum.height());
        smallOk = smallOk && minimum.width() <= 170 && minimum.height() <= 140;
        if (!shots.isEmpty()) {
            dock->setFloating(true);
            dock->resize(170, 120);
            QCoreApplication::processEvents();
            dock->grab().save(QString(shots).replace(QStringLiteral(".png"),
                                                      QStringLiteral("_%1.png").arg(dock->objectName())));
            dock->setFloating(false);
        }
    }
    window->resetLayout();
    QCoreApplication::processEvents();
    std::printf("  linea del tempo riagganciata a tutta larghezza %s, incrocio %s, pannelli piccoli %s\n",
                fullWidth ? "OK" : "NO", cornerOk ? "OK" : "NO", smallOk ? "OK" : "NO");
    docksOk = docksOk && fullWidth && redockOk && cornerOk && smallOk;
    std::printf("Pannelli: staccati e riagganciati %s, disposizione salvata e ripristinata %s, ripristino iniziale %s\n",
                docksOk ? "OK" : "NON riuscito", changed && restored ? "OK" : "NON riuscita",
                reset ? "OK" : "NON riuscito");
    std::fflush(stdout);
    return (docksOk || !player->hasMedia()) && changed && restored && reset;
}

// File trascinati da Esplora file (simulati con QMimeData): sulla linea del tempo del montaggio, in fondo, il file
// (già nel progetto) va intero nella sequenza senza un doppione nell'elenco; poi una cartella sulla Sorgente.
// Ogni trascinamento si annulla in un colpo solo.
bool checkFileDrops(SmokeRun& run)
{
    MainWindow* window = run.window;
    SequenceTimeline* timeline = TestAccess::sequenceTimeline(*window);
    const auto& seq = window->project().sequence();
    const QString source = QString::fromStdString(window->project().clips().front().path);
    const std::size_t pieces = seq.fragments().size();
    const std::size_t clips = window->project().clips().size();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(source)});
    // Linea del tempo rimpicciolita oltre "Adatta": lo spazio vuoto a destra vale come "in fondo".
    for (int i = 0; i < 20; ++i)
        timeline->zoomOut();
    const QPointF end(timeline->width() - 12, 30);
    QDragEnterEvent enter(end.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &enter);
    QDropEvent drop(end, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &drop);
    const bool inserted = enter.isAccepted() && drop.isAccepted() && seq.fragments().size() == pieces + 1
                       && seq.fragments().back().clipId == seq.fragments().front().clipId
                       && window->project().clips().size() == clips;
    timeline->fitAll();
    const QString label = window->undoText();
    const bool undone = window->undo() && seq.fragments().size() == pieces;

    // Una cartella sulla Sorgente (con una copia del file e un testo, che va ignorato): si aggiunge la
    // copia, che si apre nella Sorgente, e uno solo Annulla la toglie.
    QDir folder(QDir::temp().filePath(QStringLiteral("EditorVideoProvaTrascina")));
    folder.removeRecursively();
    QDir().mkpath(folder.path());
    QFile::copy(source, folder.filePath(QStringLiteral("copia.") + QFileInfo(source).suffix()));
    QFile note(folder.filePath(QStringLiteral("note.txt")));
    if (note.open(QIODevice::WriteOnly))
        note.write("non è un video");
    note.close();
    window->dropFiles({QDir::toNativeSeparators(folder.path())}, MainWindow::DropTarget::Source);
    const std::size_t afterFolder = window->project().clips().size();
    const bool folderOk = afterFolder == clips + 1 && seq.fragments().size() == pieces
                       && TestAccess::currentFile(*window).contains(QStringLiteral("copia."));
    window->undo();
    const bool folderUndone = window->project().clips().size() == clips;
    QElapsedTimer settle;  // si lascia finire l'analisi della copia prima di cancellarla
    settle.start();
    while (settle.elapsed() < 3000 && !window->project().allAnalyzed())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::printf("Trascinamento: nel montaggio %s (\"%s\" %s), cartella: %d file in più %s\n",
                inserted ? "OK" : "NON riuscito", qPrintable(label.mid(1)), undone ? "annullato" : "NON annullato",
                static_cast<int>(afterFolder - clips), folderOk && folderUndone ? "e annullati" : "NON riuscito");
    std::fflush(stdout);
    return inserted && undone && label.contains(QStringLiteral("Aggiungi file")) && folderOk && folderUndone;
    folder.removeRecursively();
}

// Progetto: salvato e riaperto, con gli stessi pezzi e lo stesso audio tolto (e senza modifiche
// in sospeso).
bool checkProjectSaveAndOpen(SmokeRun& run)
{
    MainWindow* window = run.window;
    QElapsedTimer& analysis = run.analysis;
    const core::Sequence& sequence = window->project().sequence();
    const QString projectFile =
        QDir::temp().filePath(QStringLiteral("EditorVideoProvaProgetto") + core::kProjectExtension);
    const std::size_t pieces = sequence.fragments().size();
    const bool saved = window->saveProjectAs(projectFile, false) && !TestAccess::isModified(*window);
    const bool reopened = saved && window->openProject(projectFile, false);
    const auto& again = window->project().sequence();
    const bool same = reopened && again.fragments().size() == pieces && again.mutedRanges().size() == 1
                   && !TestAccess::isModified(*window);
    std::printf("Progetto: salvato %s, riaperto %s, %d pezzi\n", saved ? "sì" : "NO", same ? "uguale" : "DIVERSO",
                static_cast<int>(again.fragments().size()));
    std::fflush(stdout);
    QFile::remove(projectFile);
    return same;
    while (!window->project().allAnalyzed() && analysis.elapsed() < 40000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

// Anteprima del montaggio: da poco prima della prima giunzione, 1 s di riproduzione deve passare al
// pezzo successivo e avanzare col tempo giusto.
bool checkSequencePreview(SmokeRun& run)
{
    MainWindow* window = run.window;
    const core::Sequence& sequence = window->project().sequence();
    if (sequence.fragments().size() < 2)
        return true;
    auto* program = TestAccess::programMonitor(*window);
    const double join = sequence.startOf(1);
    const double from = std::max(0.0, join - 0.3);
    program->seekTo(from);
    program->togglePlayPause();
    QElapsedTimer playing;
    playing.start();
    while (playing.elapsed() < 1000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const double reached = program->position();
    const int piece = program->currentIndex();
    if (program->isPlaying())
        program->togglePlayPause();
    const bool crossed = piece >= 1 && reached > join && reached < from + 1.6;
    std::printf("Anteprima del montaggio: da %.3f s a %.3f s, pezzo %d (giunzione a %.3f s)%s\n", from,
                reached, piece + 1, join, crossed ? "" : "  <-- NON RIUSCITA");
    std::fflush(stdout);
    return crossed;
}

// Spazi vuoti: l'ultimo pezzo trascinato col mouse 1 s più avanti lascia uno spazio; Annulla e Ripeti;
// l'anteprima passa lo spazio (nero) e arriva al pezzo; Canc sullo spazio selezionato lo chiude.
bool checkGaps(SmokeRun& run)
{
    MainWindow* window = run.window;
    const core::Sequence& sequence = window->project().sequence();
    if (sequence.fragments().size() < 2)
        return true;
    auto* line = TestAccess::sequenceTimeline(*window);
    auto* program = TestAccess::programMonitor(*window);
    line->fitAll();
    QCoreApplication::processEvents();
    const int last = static_cast<int>(sequence.fragments().size()) - 1;
    const double lastStart = sequence.startOf(last);
    const double total = sequence.totalLength();
    const double y = TestAccess::videoLaneCenter(*line);
    const double x0 = TestAccess::xForSequenceTime(*line, lastStart + sequence.fragments().back().length() / 2);
    const double x1 = x0 + (TestAccess::xForSequenceTime(*line, lastStart + 1.0) - TestAccess::xForSequenceTime(*line, lastStart));
    const auto mouse = [&](QEvent::Type type, double x, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(line, &e);
    };
    mouse(QEvent::MouseButtonPress, x0, Qt::LeftButton);
    mouse(QEvent::MouseMove, (x0 + x1) / 2, Qt::LeftButton);
    mouse(QEvent::MouseMove, x1, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, x1, Qt::NoButton);
    QCoreApplication::processEvents();
    const double moved = sequence.startOf(last);
    const bool dragged = sequence.gapCount() == 1 && std::abs(moved - (lastStart + 1.0)) < 0.05
                      && std::abs(sequence.startOf(last - 1) - (lastStart - sequence.fragments()[last - 1].length())) < 1e-6;
    const bool undone = window->undo() && sequence.gapCount() == 0 && std::abs(sequence.startOf(last) - lastStart) < 1e-6;
    const bool redone = window->redo() && sequence.gapCount() == 1;

    program->seekTo(lastStart + 0.2);  // nello spazio
    const bool inGap = TestAccess::inGap(*program) && program->currentIndex() == -1;
    program->togglePlayPause();
    const bool started = program->isPlaying();
    // Fino all'arrivo nel pezzo dopo lo spazio (1 s di spazio da attraversare), al massimo 6 s: su un PC lento
    // (es. la build automatica, senza scheda video) un giro degli eventi può durare a lungo.
    QElapsedTimer playing;
    playing.start();
    while (playing.elapsed() < 6000 && (program->currentIndex() != last || program->position() < moved + 0.05))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const double elapsed = playing.elapsed() / 1000.0;
    const double reached = program->position();
    const int piece = program->currentIndex();
    if (program->isPlaying())
        program->togglePlayPause();
    // Il tempo scorre come quello vero (con un po' di margine per il passaggio al pezzo).
    const bool crossed = inGap && started && piece == last && reached > moved
                      && reached < lastStart + 0.2 + elapsed + 0.6;
    std::printf("  spazio vuoto: partita %s, %.2f s per arrivare a %.3f s\n", started ? "sì" : "NO", elapsed, reached);

    // Come con un clic sullo spazio: il fuoco va alla linea del tempo (con il fuoco sull'elenco dei file Canc
    // toglierebbe il file).
    line->setFocus(Qt::MouseFocusReason);
    line->setSelectedGap(sequence.fragments()[last].id);
    if (const QString image = qEnvironmentVariable("EDITORVIDEO_SMOKE_GAP_SCREENSHOT"); !image.isEmpty())
        line->grab().save(image);  // la linea del tempo con lo spazio selezionato
    window->deleteSelection();
    const bool closed = sequence.gapCount() == 0 && std::abs(sequence.totalLength() - total) < 1e-6;
    std::printf("Spazi vuoti: trascinato %s (%.3f -> %.3f s), annulla %s, ripeti %s, anteprima nello spazio %s "
                "(arrivata a %.3f s, pezzo %d), chiuso con Canc %s\n",
                dragged ? "sì" : "NO", lastStart, moved, undone ? "sì" : "NO", redone ? "sì" : "NO",
                crossed ? "sì" : "NO", reached, piece + 1, closed ? "sì" : "NO");
    std::fflush(stdout);
    return dragged && undone && redone && crossed && closed;
}

// Bordi dei pezzi: la fine del primo pezzo trascinata 2 fotogrammi indietro (resta uno spazio, il pezzo dopo
// non si muove), poi con Alt 2 fotogrammi avanti (il pezzo dopo scorre); Annulla torna ogni volta com'era.
bool checkPieceEdges(SmokeRun& run)
{
    MainWindow* window = run.window;
    const core::Sequence& sequence = window->project().sequence();
    if (sequence.fragments().size() < 2)
        return true;
    auto* line = TestAccess::sequenceTimeline(*window);
    TestAccess::programMonitor(*window)->seekTo(0.0);  // il cursore lontano dai bordi: niente aggancio
    line->fitAll();
    QCoreApplication::processEvents();
    const double frame = window->frameDurationOf(sequence.fragments()[0].clipId);
    const double end = sequence.fragments()[0].end;
    const double nextStart = sequence.startOf(1);
    const double y = TestAccess::videoLaneCenter(*line);
    const double pixelsPerSecond = TestAccess::xForSequenceTime(*line, 1.0) - TestAccess::xForSequenceTime(*line, 0.0);
    const auto dragEdge = [&](double seconds, Qt::KeyboardModifiers modifiers) {
        const double x0 = line->xForBlockEdge(0, false) - 1;
        const double x1 = x0 + seconds * pixelsPerSecond;
        const auto mouse = [&](QEvent::Type type, double x, Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, modifiers);
            QCoreApplication::sendEvent(line, &e);
        };
        mouse(QEvent::MouseButtonPress, x0, Qt::LeftButton);
        mouse(QEvent::MouseMove, (x0 + x1) / 2, Qt::LeftButton);
        mouse(QEvent::MouseMove, x1, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, x1, Qt::NoButton);
        QCoreApplication::processEvents();
    };
    dragEdge(-2 * frame, Qt::NoModifier);
    const double shortened = sequence.fragments()[0].end;
    const bool shortenOk = std::abs(shortened - (end - 2 * frame)) < 1e-6 && std::abs(sequence.startOf(1) - nextStart) < 1e-6
                        && sequence.gapCount() == 1;
    const bool undone = window->undo() && std::abs(sequence.fragments()[0].end - end) < 1e-6 && sequence.gapCount() == 0;
    dragEdge(2 * frame, Qt::AltModifier);
    const double lengthened = sequence.fragments()[0].end;
    const bool rippleOk = std::abs(lengthened - (end + 2 * frame)) < 1e-6
                       && std::abs(sequence.startOf(1) - (nextStart + 2 * frame)) < 1e-6;
    const bool rippleUndone = window->undo() && std::abs(sequence.fragments()[0].end - end) < 1e-6
                           && std::abs(sequence.startOf(1) - nextStart) < 1e-6;
    std::printf("Bordi dei pezzi: accorciato %s (%.3f -> %.3f s, pezzo dopo fermo), con Alt allungato %s (%.3f s, "
                "pezzo dopo spostato), annulla %s\n",
                shortenOk ? "sì" : "NO", end, shortened, rippleOk ? "sì" : "NO", lengthened,
                undone && rippleUndone ? "sì" : "NO");
    std::fflush(stdout);
    return shortenOk && undone && rippleOk && rippleUndone;
}

// Tracce audio (step 18): "+" aggiunge A2 e A3, l'audio del file va su A2, trascinato col mouse su A3, accorciato,
// diviso con S e tolto con Canc; "−" toglie una traccia; Annulla riporta tutto com'era.
bool checkAudioTracks(SmokeRun& run)
{
    MainWindow* window = run.window;
    const core::Sequence& sequence = window->project().sequence();
    auto* line = TestAccess::sequenceTimeline(*window);
    const core::AudioTracks& audio = window->project().audioTracks();
    line->setSelectedTrack(core::AudioTracks::kOriginalTrack);  // con un pezzo video scelto "+" aggiungerebbe un livello video
    TestAccess::addTrackButton(*line)->click();
    TestAccess::addTrackButton(*line)->click();
    QCoreApplication::processEvents();
    const bool added = audio.tracks().size() == 2 && TestAccess::audioLanes(*line).size() == 3;
    const int a2 = added ? audio.tracks()[0].id : 0;
    const int a3 = added ? audio.tracks()[1].id : 0;
    const int clips = window->addAudioFiles({TestAccess::currentFile(*window)}, a2, 0.25);
    const core::AudioClip* clip = added && !audio.tracks()[0].clips.empty() ? &audio.tracks()[0].clips.front() : nullptr;
    const bool placed = clips == 1 && clip != nullptr && std::abs(clip->position - 0.25) < 1e-6;
    const int clipId = clip != nullptr ? clip->id : 0;
    const double length = clip != nullptr ? clip->length() : 0.0;
    // Trascinata col mouse dal centro della clip su A3, 0,5 s più avanti.
    TestAccess::programMonitor(*window)->seekTo(0.0);
    line->fitAll();
    QCoreApplication::processEvents();
    const double pixelsPerSecond = TestAccess::xForSequenceTime(*line, 1.0) - TestAccess::xForSequenceTime(*line, 0.0);
    const double x0 = TestAccess::xForSequenceTime(*line, 0.25 + std::min(length / 2, 0.4));
    const double x1 = x0 + 0.5 * pixelsPerSecond;
    const auto mouse = [&](QEvent::Type type, double x, double y, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(line, &e);
    };
    mouse(QEvent::MouseButtonPress, x0, TestAccess::audioLaneCenter(*line, 1) - 6, Qt::LeftButton);
    mouse(QEvent::MouseMove, (x0 + x1) / 2, TestAccess::audioLaneCenter(*line, 2), Qt::LeftButton);
    mouse(QEvent::MouseMove, x1, TestAccess::audioLaneCenter(*line, 2), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, x1, TestAccess::audioLaneCenter(*line, 2), Qt::NoButton);
    QCoreApplication::processEvents();
    const core::AudioClip* moved = audio.find(clipId);
    const bool dragged = moved != nullptr && audio.trackOf(clipId) == a3 && std::abs(moved->position - 0.75) < 0.15;
    // Accorciata di 0,2 s alla fine, divisa a metà con S, la seconda parte tolta con Canc.
    const bool trimmed = moved != nullptr && window->trimAudioClip(clipId, false, moved->end - 0.2, false)
                      && std::abs(audio.find(clipId)->length() - (length - 0.2)) < 1e-6;
    // Il cursore del montaggio non va oltre la fine dei pezzi video: la clip torna all'inizio e si divide dentro
    // quella parte.
    const bool back = window->moveAudioClip(clipId, a3, 0.0);
    const double middle = std::min(length - 0.2, sequence.totalLength()) / 2;
    line->setSelectedAudioClip(clipId);
    // Il monitor arriva al punto quando il fotogramma è decodificato: si aspetta che ci sia.
    TestAccess::programMonitor(*window)->seekTo(middle);
    QElapsedTimer seeking;
    seeking.start();
    while (std::abs(TestAccess::programMonitor(*window)->position() - middle) > 0.1 && seeking.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const double splitAt = TestAccess::programMonitor(*window)->position();
    window->splitAtPlayhead();
    const bool split = back && audio.track(a3) != nullptr && audio.track(a3)->clips.size() == 2;
    if (!split)
        std::printf("  divisione: cursore a %.3f s (voluto %.3f s), clip audio scelta %d\n", splitAt, middle,
                    line->selectedAudioClip());
    line->setFocus(Qt::MouseFocusReason);
    if (split)
        line->setSelectedAudioClip(audio.track(a3)->clips[1].id);
    window->deleteSelection();
    const bool deleted = audio.track(a3) != nullptr && audio.track(a3)->clips.size() == 1;
    // Livello, muto e solo dalle corsie.
    window->setTrackLevel(a3, -6.0);
    window->setTrackSolo(a3, true);
    const bool mix = audio.mix(a3).levelDb == -6.0 && !audio.audible(core::AudioTracks::kOriginalTrack) && audio.audible(a3);
    if (const QString image = qEnvironmentVariable("EDITORVIDEO_SMOKE_TRACKS_SCREENSHOT"); !image.isEmpty())
        line->grab().save(image);
    // "−" toglie la traccia scelta (A3, quella dell'ultima clip toccata).
    TestAccess::removeTrackButton(*line)->click();
    QCoreApplication::processEvents();
    const bool removed = audio.tracks().size() == 1 && audio.track(a3) == nullptr && TestAccess::audioLanes(*line).size() == 2;
    // Annulla tutto: tracce e clip spariscono, il montaggio torna com'era.
    bool undone = true;
    for (int i = 0; i < 12 && !audio.isDefault(); ++i)
        undone = window->undo() && undone;
    undone = undone && audio.isDefault() && TestAccess::audioLanes(*line).size() == 1;
    std::printf("Tracce audio: aggiunte %s, file su A2 %s, trascinato su A3 %s, accorciato %s, diviso %s, tolto con "
                "Canc %s, livello e solo %s, traccia tolta %s, annulla %s\n",
                added ? "sì" : "NO", placed ? "sì" : "NO", dragged ? "sì" : "NO", trimmed ? "sì" : "NO",
                split ? "sì" : "NO", deleted ? "sì" : "NO", mix ? "sì" : "NO", removed ? "sì" : "NO",
                undone ? "sì" : "NO");
    std::fflush(stdout);
    return added && placed && dragged && trimmed && split && deleted && mix && removed && undone;
}

// Scritte (step T3): T mette una scritta al cursore, S la divide, Canc toglie la seconda parte; la prima si
// trascina col mouse su un altro livello e si accorcia dal bordo; Ctrl+clic la mette in un gruppo con un'altra;
// il livello si toglie; Annulla riporta tutto com'era.
bool checkTexts(SmokeRun& run)
{
    MainWindow* window = run.window;
    bool ok = true;
    auto* line = TestAccess::sequenceTimeline(*window);
    const core::TextTracks& text = window->project().textTracks();
    TestAccess::programMonitor(*window)->seekTo(0.0);
    const int first = window->addTextClip(false);
    const bool added = first != 0 && text.layers().size() == 1 && line->textLanes().size() == 1
                    && line->selectedTextClip() == first && std::abs(text.find(first)->length - 5.0) < 1e-6;
    // Divisa dentro la scritta e (come per le clip audio) con il cursore dove arriva il montaggio.
    const double middle = std::clamp(std::min(window->project().sequence().totalLength(), 4.0) / 2, 0.3, 2.0);
    TestAccess::programMonitor(*window)->seekTo(middle);
    QElapsedTimer seeking;
    seeking.start();
    while (std::abs(TestAccess::programMonitor(*window)->position() - middle) > 0.1 && seeking.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const double splitAt = TestAccess::programMonitor(*window)->position();
    line->setSelectedTextClip(first);
    window->splitAtPlayhead();
    const bool split = added && text.layers()[0].clips.size() == 2
                    && std::abs(text.find(first)->length - splitAt) < 1e-6;
    if (split)
        line->setSelectedTextClip(text.layers()[0].clips[1].id);
    window->deleteSelection();
    const bool deleted = split && text.layers()[0].clips.size() == 1;
    // Un secondo livello e la scritta trascinata lì, 0,5 s più avanti.
    const int second = window->addTextLayer();
    line->fitAll();
    QCoreApplication::processEvents();
    const double pixelsPerSecond = TestAccess::xForSequenceTime(*line, 1.0) - TestAccess::xForSequenceTime(*line, 0.0);
    const double length = deleted ? text.find(first)->length : 0.0;
    const double x0 = TestAccess::xForSequenceTime(*line, length / 2);
    const double x1 = x0 + 0.5 * pixelsPerSecond;
    const auto mouse = [&](QEvent::Type type, double x, double y, Qt::MouseButtons buttons,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, modifiers);
        QCoreApplication::sendEvent(line, &e);
    };
    const bool lanes = line->textLanes().size() == 2 && TestAccess::textLaneCenter(*line, 1) < TestAccess::textLaneCenter(*line, 0)
                    && TestAccess::textLaneCenter(*line, 0) < TestAccess::videoLaneCenter(*line);
    mouse(QEvent::MouseButtonPress, x0, TestAccess::textLaneCenter(*line, 0), Qt::LeftButton);
    mouse(QEvent::MouseMove, (x0 + x1) / 2, TestAccess::textLaneCenter(*line, 1), Qt::LeftButton);
    mouse(QEvent::MouseMove, x1, TestAccess::textLaneCenter(*line, 1), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, x1, TestAccess::textLaneCenter(*line, 1), Qt::NoButton);
    QCoreApplication::processEvents();
    const core::TextClip* moved = deleted ? text.find(first) : nullptr;
    const bool dragged = moved != nullptr && text.layerOf(first) == second && std::abs(moved->position - 0.5) < 0.2;
    // La fine accorciata con il mouse dal bordo destro.
    const double end = moved != nullptr ? moved->endPosition() : 0.0;
    const double shorten = std::min(0.2, length / 3);
    const double xEnd = TestAccess::xForSequenceTime(*line, end);
    const double y1 = TestAccess::textLaneCenter(*line, 1);
    mouse(QEvent::MouseButtonPress, xEnd - 1, y1, Qt::LeftButton);
    mouse(QEvent::MouseMove, xEnd - 1 - shorten * pixelsPerSecond, y1, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, xEnd - 1 - shorten * pixelsPerSecond, y1, Qt::NoButton);
    QCoreApplication::processEvents();
    const bool trimmed = moved != nullptr && text.find(first) != nullptr && text.find(first)->endPosition() < end - 0.05;
    // Una seconda scritta sul primo livello, e le due in gruppo con Ctrl+clic e G: si muovono insieme.
    TestAccess::programMonitor(*window)->seekTo(0.0);
    line->setSelectedTextLayer(text.layers()[0].id);
    const int third = deleted ? window->addTextClip(false) : 0;
    const bool placed = third != 0 && text.layerOf(third) == text.layers()[0].id;
    mouse(QEvent::MouseButtonPress, TestAccess::xForSequenceTime(*line, text.find(third) != nullptr ? text.find(third)->position + 0.5 : 0.0),
          TestAccess::textLaneCenter(*line, 0), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, TestAccess::xForSequenceTime(*line, text.find(third) != nullptr ? text.find(third)->position + 0.5 : 0.0),
          TestAccess::textLaneCenter(*line, 0), Qt::NoButton);
    mouse(QEvent::MouseButtonPress, TestAccess::xForSequenceTime(*line, moved != nullptr && text.find(first) != nullptr ? text.find(first)->position + 0.1 : 0.0),
          TestAccess::textLaneCenter(*line, 1), Qt::LeftButton, Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, TestAccess::xForSequenceTime(*line, moved != nullptr && text.find(first) != nullptr ? text.find(first)->position + 0.1 : 0.0),
          TestAccess::textLaneCenter(*line, 1), Qt::NoButton, Qt::ControlModifier);
    const bool multi = line->selectedObjects().size() == 2;
    window->groupSelection();
    // Il gruppo compare nella scheda Gruppi, con le due scritte.
    const std::vector<GroupRow>& listedGroups = TestAccess::groupPanel(*window)->rows();
    const bool listed = std::any_of(listedGroups.begin(), listedGroups.end(), [&](const GroupRow& row) {
        return row.id > 0 && row.entries.size() == 2
            && std::any_of(row.entries.begin(), row.entries.end(),
                           [&](const GroupEntry& e) { return e.ref == core::ObjectRef{core::ObjectRef::Kind::TextClip, third}; });
    });
    // Un clic sul gruppo nell'elenco sceglie sulla linea del tempo tutti i suoi oggetti.
    bool fromList = false;
    if (listed) {
        line->setSelected(0);
        line->setSelectedTextClip(0);
        QTreeWidget* tree = TestAccess::groupPanel(*window)->tree();
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const GroupRow& row = TestAccess::groupPanel(*window)->rows()[static_cast<std::size_t>(i)];
            if (row.id <= 0 || row.entries.size() != 2)
                continue;
            tree->itemClicked(tree->topLevelItem(i), 0);
            fromList = line->selectedObjects().size() == 2;
            // E ciò che è scelto sulla linea del tempo si evidenzia nell'elenco (il nodo del gruppo).
            fromList = fromList && tree->currentItem() == tree->topLevelItem(i);
            break;
        }
    }
    const bool grouped = placed && listed && fromList && window->project().groupMembers({core::ObjectRef::Kind::TextClip, third}).size() == 2;
    // Il livello scelto si toglie con le sue scritte.
    const int layersBefore = static_cast<int>(text.layers().size());
    line->setSelectedTextLayer(second);
    window->removeSelectedTextLayer();
    const bool removed = layersBefore == 2 && text.layers().size() == 1 && text.find(first) == nullptr;
    // L'editor (step T4): testo, stile e durata si cambiano in un colpo solo e Annulla li riporta com'erano.
    bool edited = false;
    if (const core::TextClip* now = text.find(third)) {
        const core::TextClip original = *now;
        core::TextClip changed = original;
        changed.text = "Titolo\nsecondo rigo";
        changed.style.bold = true;
        changed.style.color = {255, 0, 0, 255};
        changed.length = original.length - 1.0;
        const bool applied = window->applyTextEdit(changed);
        const bool shown = applied && text.find(third)->text == changed.text && text.find(third)->style == changed.style
                        && std::abs(text.find(third)->length - changed.length) < 1e-6
                        && line->textLanes()[0].clips[0].label.contains(QStringLiteral("…"));
        const bool back = window->undo() && *text.find(third) == original;
        // Dissolvenze (step T7): si impostano con la stessa modifica e Annulla le toglie.
        core::TextClip faded = original;
        faded.fadeIn = std::min(0.3, original.length / 3);
        faded.fadeOut = faded.fadeIn;
        const bool fadesSet = window->applyTextEdit(faded) && std::abs(text.find(third)->fadeIn - faded.fadeIn) < 1e-6
                           && std::abs(text.find(third)->fadeOut - faded.fadeOut) < 1e-6;
        const bool fadesBack = window->undo() && *text.find(third) == original;
        edited = shown && back && fadesSet && fadesBack;
    }
    // Il pannello della scritta (pagina del pannello Proprietà): le modifiche valgono subito e scrivere una frase
    // di fila è una sola voce di Annulla.
    bool panel = false;
    if (text.find(third) != nullptr) {
        const core::TextClip original = *text.find(third);
        window->showTextPanel(third, false);
        auto* edit = TestAccess::textPanel(*window)->findChild<QPlainTextEdit*>("testo");
        auto* sizeBox = TestAccess::textPanel(*window)->findChild<QDoubleSpinBox*>("grandezza");
        if (edit != nullptr && sizeBox != nullptr && TestAccess::hasClip(*TestAccess::textPanel(*window))
            && TestAccess::showingText(*TestAccess::propertiesPanel(*window)) && TestAccess::textPanel(*window)->isVisibleTo(TestAccess::propertiesPanel(*window))) {
            edit->setPlainText(QStringLiteral("Prima"));
            edit->setPlainText(QStringLiteral("Prima e dopo"));
            sizeBox->setValue(9.0);
            const bool applied = text.find(third) != nullptr && text.find(third)->text == "Prima e dopo"
                              && std::abs(text.find(third)->style.size - 0.09) < 1e-9;
            const bool oneEntry = window->undo() && text.find(third) != nullptr && *text.find(third) == original
                               && TestAccess::textPanel(*window)->clip() == original;
            panel = applied && oneEntry;
        }
    }
    std::printf("Scritte: editor nel pannello Proprietà, modifiche subito e in una sola voce di Annulla %s\n",
                panel ? "sì" : "NO");
    std::fflush(stdout);
    ok = ok && panel;
    // Anteprima (step T5): al tempo della scritta il monitor la mostra, fuori no, e la modifica si vede subito.
    bool onMonitor = false;
    if (const core::TextClip* now = text.find(third)) {
        auto* monitor = TestAccess::programMonitor(*window);
        window->trimTextClip(third, false, now->position + 0.3);  // corta, così il montaggio di prova la supera
        now = text.find(third);
        monitor->seekTo(now->position + 0.1);
        const std::vector<core::TextClip> during = monitor->visibleText();
        const bool shown = std::any_of(during.begin(), during.end(), [&](const core::TextClip& c) { return c.id == third; });
        monitor->seekTo(now->endPosition() + 0.1);
        const std::vector<core::TextClip> after = monitor->visibleText();
        const bool hidden = std::none_of(after.begin(), after.end(), [&](const core::TextClip& c) { return c.id == third; });
        monitor->seekTo(now->position + 0.1);
        QCoreApplication::processEvents();
        const QImage grabbed = monitor->grab().toImage();
        // Trascinata sull'immagine: il nuovo centro si vede subito nel monitor, e Annulla lo riporta com'era.
        const double oldX = text.find(third)->style.centerX;
        const bool movedOnScreen = window->moveTextOnScreen(third, 0.25, 0.4) && std::abs(text.find(third)->style.centerX - 0.25) < 1e-9;
        const std::vector<core::TextClip> movedText = monitor->visibleText();
        const bool seenMoved = !movedText.empty() && std::abs(movedText.back().style.centerY - 0.4) < 1e-9;
        const bool movedBack = window->undo() && std::abs(text.find(third)->style.centerX - oldX) < 1e-9;
        onMonitor = shown && hidden && !grabbed.isNull() && TestAccess::renderCount(TestAccess::textOverlay(*monitor)) > 0 && movedOnScreen
                 && seenMoved && movedBack;
    }
    // Altezze (con scritte in più o no): video e audio hanno la stessa altezza, le scritte sono più basse.
    bool uniform = std::abs(TestAccess::videoLaneHeight(*line) - TestAccess::audioLaneHeight(*line)) < 1.0;
    for (int tall : {320, 600}) {
        line->resize(line->width(), tall);
        QCoreApplication::processEvents();
        uniform = uniform && std::abs(TestAccess::videoLaneHeight(*line) - TestAccess::audioLaneHeight(*line)) < 1.0;
    }
    std::printf("Scritte: video e audio alti uguali %s\n", uniform ? "sì" : "NO");
    std::fflush(stdout);
    ok = ok && uniform;
    // Annulla tutto: i livelli e le scritte spariscono.
    bool undone = true;
    for (int i = 0; i < 16 && !text.layers().empty(); ++i)
        undone = window->undo() && undone;
    undone = undone && text.layers().empty() && line->textLanes().empty();
    std::printf("Scritte: aggiunta %s, divisa %s, tolta con Canc %s, due livelli %s, trascinata sull'altro livello %s, "
                "accorciata %s, Ctrl+clic e gruppo %s, livello tolto %s, editor %s, anteprima %s, annulla %s\n",
                added ? "sì" : "NO", split ? "sì" : "NO", deleted ? "sì" : "NO", lanes ? "sì" : "NO",
                dragged ? "sì" : "NO", trimmed ? "sì" : "NO", multi && grouped ? "sì" : "NO", removed ? "sì" : "NO",
                edited ? "sì" : "NO", onMonitor ? "sì" : "NO", undone ? "sì" : "NO");
    std::fflush(stdout);
    ok = ok && added && split && deleted && lanes && dragged && trimmed && multi && grouped && removed && edited && onMonitor && undone;
    return ok;
}

// Transizioni (X5): si lascia una transizione sulla giunzione, si allarga, si toglie con Canc, si annulla.
bool checkTransitions(SmokeRun& run)
{
    MainWindow* window = run.window;
    bool ok = true;
    const core::Project& proj = window->project();
    auto* line = TestAccess::sequenceTimeline(*window);
    line->resize(line->width(), std::max(line->height(), 320));
    TestAccess::programMonitor(*window)->seekTo(0.0);
    line->fitAll();
    QCoreApplication::processEvents();
    const std::size_t piecesBefore = proj.sequence().fragments().size();
    const QString restoreFile =
        QDir::temp().filePath(QStringLiteral("EditorVideoProvaTransizioniPrima") + core::kProjectExtension);
    const bool restorable = window->saveProjectAs(restoreFile, false);
    const auto firstJunction = [&]() {
        const auto& frags = proj.sequence().fragments();
        for (std::size_t i = 0; i + 1 < frags.size(); ++i)
            if (proj.isJunction(frags[i].id))
                return static_cast<int>(i);
        return -1;
    };
    // Fra file interi non c'è margine: la giunzione si fa dividendo il primo pezzo a metà (le due metà hanno
    // margine l'una nell'altra).
    int junction = -1;
    {
        const double middle = proj.sequence().fragments().front().length() / 2;
        TestAccess::programMonitor(*window)->seekTo(middle);
        QElapsedTimer seeking;
        seeking.start();
        while (std::abs(TestAccess::programMonitor(*window)->position() - middle) > 0.1 && seeking.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        window->splitAtPlayhead();
        junction = firstJunction();
    }
    line->fitAll();
    QCoreApplication::processEvents();
    const bool found = junction >= 0;
    bool dropped = false, refusedElsewhere = false, widened = false, selected = false, removed = false, redone = false;
    bool undone = false;
    bool onMonitor = false;
    bool reopenedKept = false;
    if (found) {
        const int beforeId = proj.sequence().fragments()[static_cast<std::size_t>(junction)].id;
        const double junctionX = TestAccess::xForJunction(*line, junction);
        const double y = TestAccess::videoLaneCenter(*line);
        const auto dropAt = [&](double x) {
            QMimeData data;
            data.setData(QLatin1String(kTransitionMime), QByteArrayLiteral("dissolvenza"));
            const QPoint at(static_cast<int>(x), static_cast<int>(y));
            QDragEnterEvent enter(at, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(line, &enter);
            QDropEvent drop(QPointF(at), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(line, &drop);
            QCoreApplication::processEvents();
        };
        // Lontano da ogni giunzione: niente.
        dropAt(TestAccess::xForSequenceTime(*line, 0.0));
        refusedElsewhere = proj.transitions().empty();
        dropAt(junctionX + 5);
        const core::TransitionClip* added = proj.transitions().atJunction(beforeId);
        dropped = added != nullptr && added->type == "dissolvenza" && added->duration >= core::kMinTransition
               && line->transitions().size() == 1;
        if (dropped) {
            const int id = added->id;
            const double before = added->duration;
            // Il monitor del montaggio: dentro il tratto mostra i due pezzi fusi, fuori il lettore; cursore fermo e
            // in riproduzione attraverso la transizione.
            {
                ProgramMonitor* monitor = TestAccess::programMonitor(*window);
                const core::TransitionPlacement placed = *proj.transitionPlacement(id);
                monitor->seekTo(placed.junction);
                const bool inside = TestAccess::transitionShown(*monitor) == id && TestAccess::transitionFrame(*monitor) != nullptr
                                 && TestAccess::transitionFrame(*monitor)->width > 0 && TestAccess::transitionFrame(*monitor)->width <= 1280
                                 && TestAccess::transitionFrame(*monitor)->height <= 720;
                monitor->seekTo(std::max(0.0, placed.start() - 0.05));
                const bool beforeSpan = placed.start() < 0.05 || TestAccess::transitionShown(*monitor) == 0;
                monitor->seekTo(std::min(proj.sequence().totalLength(), placed.end() + 0.02));
                const bool afterSpan = placed.end() + 0.02 > proj.sequence().totalLength() || TestAccess::transitionShown(*monitor) == 0;
                // In riproduzione, da poco prima.
                monitor->seekTo(std::max(0.0, placed.start() - 0.1));
                monitor->setPlaying(true);
                bool seen = false;
                QElapsedTimer playing;
                playing.start();
                while (playing.elapsed() < 4000 && monitor->isPlaying() && monitor->position() < placed.end() + 0.1) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    seen = seen || TestAccess::transitionShown(*monitor) == id;
                }
                const bool through = seen && monitor->position() >= placed.end() - 0.02
                                  && (TestAccess::transitionShown(*monitor) == 0 || !monitor->isPlaying());
                monitor->setPlaying(false);
                std::printf("  monitor: dentro il tratto %s, prima %s, dopo %s, in riproduzione attraverso %s (posizione %.3f s, "
                            "transizione da %.3f a %.3f s)\n",
                            inside ? "sì" : "NO", beforeSpan ? "sì" : "NO", afterSpan ? "sì" : "NO", through ? "sì" : "NO",
                            monitor->position(), placed.start(), placed.end());
                onMonitor = inside && beforeSpan && afterSpan && through;
                monitor->seekTo(0.0);
            }
            const auto mouse = [&](QEvent::Type type, double x, Qt::MouseButtons buttons) {
                QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                QCoreApplication::sendEvent(line, &e);
            };
            mouse(QEvent::MouseButtonPress, junctionX, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, junctionX, Qt::NoButton);
            selected = line->selectedTransition() == id;
            // Si stringe a metà (almeno il minimo) trascinando il bordo destro: i file di prova sono brevi.
            const double target = std::max(core::kMinTransition, before / 2);
            const double perSecond = TestAccess::xForSequenceTime(*line, 1.0) - TestAccess::xForSequenceTime(*line, 0.0);
            const double edge = junctionX + before / 2 * perSecond;
            mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton);
            mouse(QEvent::MouseMove, (edge + junctionX) / 2, Qt::LeftButton);
            mouse(QEvent::MouseMove, junctionX + target / 2 * perSecond, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, junctionX + target / 2 * perSecond, Qt::NoButton);
            QCoreApplication::processEvents();
            const core::TransitionClip* now = proj.transitions().find(id);
            widened = now != nullptr && std::abs(now->duration - target) < 0.03 && now->duration < before;
            line->setFocus(Qt::MouseFocusReason);
            line->setSelectedTransition(id);
            window->deleteSelection();
            removed = proj.transitions().empty() && line->transitions().empty();
            redone = window->undo() && proj.transitions().find(id) != nullptr;  // rimessa
            window->undo();  // durata di prima
            window->undo();  // aggiunta
            undone = proj.transitions().empty() && proj.sequence().fragments().size() >= piecesBefore;
            // Salvata e riaperta: la transizione c'è ancora, e si vede sulla timeline appena i file sono analizzati.
            const QString reopenFile =
                QDir::temp().filePath(QStringLiteral("EditorVideoProvaTransizioni") + core::kProjectExtension);
            dropAt(junctionX + 5);
            const bool savedWith = proj.transitions().clips().size() == 1 && window->saveProjectAs(reopenFile, false);
            reopenedKept = savedWith && window->openProject(reopenFile, false);
            QElapsedTimer analyzing;
            analyzing.start();
            while (!window->project().allAnalyzed() && analyzing.elapsed() < 20000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QCoreApplication::processEvents();
            reopenedKept = reopenedKept && window->project().transitions().clips().size() == 1
                        && TestAccess::sequenceTimeline(*window)->transitions().size() == 1;
            QFile::remove(reopenFile);
            // Il progetto com'era prima della prova.
            window->openProject(restoreFile, false);
            while (!window->project().allAnalyzed() && analyzing.elapsed() < 40000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }
    // Se la prova ha diviso un pezzo, si annulla.
    while (proj.sequence().fragments().size() > piecesBefore && window->undo()) {
    }
    QFile::remove(restoreFile);
    const bool all = found && dropped && refusedElsewhere && selected && widened && removed && redone && undone && onMonitor
                  && restorable && reopenedKept;
    std::printf("Transizioni: giunzione %s, lasciata %s, rifiutata lontano %s, selezionata %s, durata col bordo %s, "
                "tolta con Canc %s, annulla %s, monitor %s, salvata e riaperta %s\n",
                found ? "sì" : "NO", dropped ? "sì" : "NO", refusedElsewhere ? "sì" : "NO", selected ? "sì" : "NO",
                widened ? "sì" : "NO", removed ? "sì" : "NO", redone && undone ? "sì" : "NO", onMonitor ? "sì" : "NO",
                reopenedKept ? "sì" : "NO");
    std::fflush(stdout);
    return ok && all;
}

// Mix e file solo audio (step 19): un file solo audio non va mai nella traccia video (né rilasciato né con A),
// e livello, muto e solo si cambiano suonando senza fermare l'anteprima.
bool checkMixAndAudioOnly(SmokeRun& run)
{
    MainWindow* window = run.window;
    const core::Sequence& sequence = window->project().sequence();
    bool ok = true;
    const core::AudioTracks& audio = window->project().audioTracks();
    const QString wav = QDir::temp().filePath(QStringLiteral("EditorVideoProvaAudio.wav"));
    {
        QFile wavFile(wav);
        const int rate = 8000;
        if (wavFile.open(QIODevice::WriteOnly)) {
            QDataStream out(&wavFile);
            out.setByteOrder(QDataStream::LittleEndian);
            out.writeRawData("RIFF", 4);
            out << quint32(36 + rate * 2);
            out.writeRawData("WAVEfmt ", 8);
            out << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
            out.writeRawData("data", 4);
            out << quint32(rate * 2);
            for (int i = 0; i < rate; ++i)
                out << qint16(8000 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));  // 1 s, 440 Hz
        }
    }
    const QString original = TestAccess::currentFile(*window);
    int originalId = 0;
    for (const core::Clip& c : window->project().clips()) {
        if (QString::fromStdString(c.path) == original || QDir::toNativeSeparators(QString::fromStdString(c.path)) == QDir::toNativeSeparators(original))
            originalId = c.id;
    }
    const std::size_t pieces = sequence.fragments().size();
    window->dropFiles({QDir::toNativeSeparators(wav)}, MainWindow::DropTarget::Sequence, 0);
    const bool dropped = sequence.fragments().size() == pieces && audio.tracks().size() == 1
                      && audio.tracks()[0].clips.size() == 1;
    QElapsedTimer analysisTimer;
    analysisTimer.start();
    while (analysisTimer.elapsed() < 20000 && !window->project().allAnalyzed())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    int wavId = 0;
    for (const core::Clip& c : window->project().clips()) {
        if (QString::fromStdString(c.path).endsWith(QStringLiteral("EditorVideoProvaAudio.wav")))
            wavId = c.id;
    }
    bool withA = false;
    if (wavId != 0 && !audio.tracks().empty()) {
        window->selectClip(wavId);
        window->addToSequence();
        withA = sequence.fragments().size() == pieces && audio.tracks()[0].clips.size() == 2;
    }
    if (originalId != 0)
        window->selectClip(originalId);
    // Livello, muto e solo mentre suona: la riproduzione non si ferma.
    TestAccess::programMonitor(*window)->seekTo(0.0);
    TestAccess::programMonitor(*window)->setPlaying(true);
    const int track = audio.tracks().empty() ? 0 : audio.tracks()[0].id;
    window->setTrackLevel(track, -12.0);
    window->setTrackSolo(track, true);
    window->setTrackMuted(core::AudioTracks::kOriginalTrack, true);
    const int musicClip = audio.tracks().empty() || audio.tracks()[0].clips.empty() ? 0 : audio.tracks()[0].clips[0].id;
    window->setEnvelope(false, musicClip, {{0.2, 0.0}, {0.8, -30.0}});  // dissolvenza: la linea del livello
    const bool keepsPlaying = TestAccess::programMonitor(*window)->isPlaying() && audio.audible(track)
                           && !audio.audible(core::AudioTracks::kOriginalTrack) && audio.tracks()[0].clips[0].envelope.size() == 2;
    TestAccess::programMonitor(*window)->setPlaying(false);
    // Il pannello Mixer ha una colonna per traccia, con gli stessi valori delle corsie, e i suoi comandi
    // cambiano il progetto come quelli della linea del tempo.
    MixerPanel* mixer = TestAccess::mixerPanel(*window);
    bool mixerOk = TestAccess::stripCount(*mixer) == 1 + audio.addedTrackCount();
    if (mixerOk) {
        const MixerPanel::Track& a1 = TestAccess::trackAt(*mixer, 0);
        const MixerPanel::Track& a2 = TestAccess::trackAt(*mixer, 1);
        mixerOk = a1.trackId == core::AudioTracks::kOriginalTrack && a1.muted && a2.trackId == track
               && a2.solo && std::abs(a2.levelDb + 12.0) < 1e-9;
        mixer->onLevel(track, -3.0);
        mixer->onMuted(core::AudioTracks::kOriginalTrack, false);
        mixer->onSolo(track, false);
        mixerOk = mixerOk && std::abs(audio.mix(track).levelDb + 3.0) < 1e-9
               && !audio.mix(core::AudioTracks::kOriginalTrack).muted && !audio.mix(track).solo
               && std::abs(TestAccess::trackAt(*mixer, 1).levelDb + 3.0) < 1e-9 && !TestAccess::trackAt(*mixer, 0).muted;
    }
    mixer->onLevel(core::kMasterTrackId, -6.0);
    mixerOk = mixerOk && std::abs(audio.masterLevelDb() + 6.0) < 1e-9 && std::abs(mixer->masterLevelDb() + 6.0) < 1e-9;
    {
        core::AudioEffect eq;
        eq.bands[2].gainDb = 2.0;
        mixer->onEffects(track, {eq});
        mixer->onEffects(core::kMasterTrackId, {eq});
        mixerOk = mixerOk && audio.effects(track).size() == 1 && TestAccess::trackAt(*mixer, 1).effects.size() == 1
               && audio.effects(core::AudioTracks::kMasterTrack).size() == 1 && audio.needsMix();
        core::AudioEffect normalize;
        normalize.kind = core::EffectKind::Normalize;
        mixer->onEffects(core::kMasterTrackId, {normalize});
        mixerOk = mixerOk && core::hasActiveNormalize(audio.effects(core::AudioTracks::kMasterTrack))
               && TestAccess::stripCount(*mixer) > 0 && audio.needsMix();
        mixer->onEffects(track, {});
        mixer->onEffects(core::kMasterTrackId, {});
        mixerOk = mixerOk && audio.effects(track).empty() && audio.effects(core::AudioTracks::kMasterTrack).empty()
               && TestAccess::trackAt(*mixer, 1).effects.empty();
    }
    mixer->onPan(track, 0.5);
    mixerOk = mixerOk && std::abs(audio.mix(track).pan - 0.5) < 1e-9 && std::abs(TestAccess::trackAt(*mixer, 1).pan - 0.5) < 1e-9
           && audio.needsMix();
    // Vu-meter: salgono subito al picco (0,5 = -6 dB) e senza altri picchi scendono piano.
    mixer->feedPeaks({{track, {0.5f, 0.25f}}, {core::kMasterTrackId, {0.25f, 0.25f}}}, 0.04);
    const bool meterUp = std::abs(TestAccess::meterDb(*mixer, 1, 0) + 6.02) < 0.1 && std::abs(TestAccess::meterDb(*mixer, 1, 1) + 12.04) < 0.1
                      && TestAccess::meterDb(*mixer, 0, 0) <= -59.9 && std::abs(TestAccess::masterMeterDb(*mixer, 0) + 12.04) < 0.1;
    mixer->feedPeaks({}, 0.5);
    const bool meterFalls = TestAccess::meterDb(*mixer, 1, 0) < -20.0 && TestAccess::meterDb(*mixer, 1, 0) > -40.0;
    TestAccess::resetMeters(*mixer);
    mixerOk = mixerOk && meterUp && meterFalls && TestAccess::meterDb(*mixer, 1, 0) <= -59.9 && TestAccess::masterMeterDb(*mixer, 0) <= -59.9;
    std::printf("Mixer: colonne come le corsie e comandi che cambiano il progetto, vu-meter %s\n", mixerOk ? "sì" : "NO");
    std::fflush(stdout);
    ok = ok && mixerOk;
    bool undone = true;
    for (int i = 0; i < 40 && !(audio.isDefault() && window->project().find(wavId) == nullptr); ++i)
        undone = window->undo() && undone;
    undone = undone && audio.isDefault() && sequence.fragments().size() == pieces;
    std::printf("Mix audio: file solo audio rilasciato nel montaggio %s, con A %s, livello/solo/muto suonando %s, annulla %s\n",
                dropped ? "su una traccia audio (non su quella video)" : "NO", withA ? "sì" : "NO",
                keepsPlaying ? "senza fermare la riproduzione" : "NO", undone ? "sì" : "NO");
    std::fflush(stdout);
    ok = ok && dropped && withA && keepsPlaying && undone;
    QFile::remove(wav);
    return ok;
}

// Esportazione dei tratti come dal menu (senza chiedere dove salvare), con il messaggio finale; alla fine si chiude
// la prova (finishSmokeTest) con `ok` e l'esito dell'esportazione.
void checkExportAndFinish(const std::shared_ptr<SmokeRun>& run, bool ok)
{
    MainWindow* window = run->window;
    const double expectedLength = TestAccess::exportLength(*window);
    const QString target = QDir::temp().filePath(QStringLiteral("EditorVideoProvaEsportazione.")
                                                  + QFileInfo(TestAccess::currentFile(*window)).suffix());
    QFile::remove(target);
    // Rallentata apposta, perché la finestra di avanzamento faccia in tempo a comparire, come con un file grande.
    if (!window->startExport(QDir::toNativeSeparators(target), true, std::chrono::milliseconds(25))) {
        std::printf("Esportazione: non partita\n");
        finishSmokeTest(*run, false);
        return;
    }
    // Quando compare il messaggio finale si aspetta ancora un po' (più del ritardo con cui compare la
    // finestra di avanzamento) e si controlla che nessuna finestra di avanzamento sia rimasta aperta.
    auto* poll = new QTimer(&run->app);
    auto messageSeen = std::make_shared<QElapsedTimer>();
    auto waiting = std::make_shared<QElapsedTimer>();
    waiting->start();
    QObject::connect(poll, &QTimer::timeout, &run->app, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            if (waiting->elapsed() < 15000)
                return;
            // Il messaggio finale non è mai arrivato in primo piano: di solito perché la finestra di
            // avanzamento è rimasta aperta sopra.
            poll->stop();
            const QWidget* modal = QApplication::activeModalWidget();
            std::printf("Esportazione: messaggio finale mai in primo piano (finestra attiva: %s, "
                        "esportazione %s, riuscita: %s)\n",
                        modal != nullptr ? qPrintable(modal->windowTitle()) : "nessuna",
                        TestAccess::isExporting(*window) ? "ancora in corso" : "finita",
                        TestAccess::lastExportSucceeded(*window) ? "sì" : qPrintable(QStringLiteral("no: ") + TestAccess::lastExportProblem(*window)));
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (w->isVisible())
                    std::printf("  finestra visibile: \"%s\" (%s)\n", qPrintable(w->windowTitle()),
                                w->metaObject()->className());
            }
            std::fflush(stdout);
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
            finishSmokeTest(*run, false);
            return;
        }
        if (!messageSeen->isValid())
            messageSeen->start();
        if (messageSeen->elapsed() < 700)
            return;
        poll->stop();
        const auto dialogs = window->findChildren<QProgressDialog*>();
        const auto stuck = std::count_if(dialogs.begin(), dialogs.end(),
                                         [](QProgressDialog* d) { return d->isVisible(); });
        bool exported = TestAccess::lastExportSucceeded(*window) && QFileInfo::exists(target);
        if (exported) {
            const double length = core::probeFile(target.toStdString()).durationSeconds;
            std::printf("File esportato: %.3f s (attesi %.3f s)\n", length, expectedLength);
            exported = std::abs(length - expectedLength) < 0.15;
        }
        std::printf("Esportazione: %s (\"%s\"), finestre di avanzamento rimaste aperte: %d\n",
                    exported ? "riuscita" : "NON riuscita", qPrintable(box->windowTitle()),
                    static_cast<int>(stuck));
        std::fflush(stdout);
        box->accept();
        QFile::remove(target);
        finishSmokeTest(*run, ok && exported && stuck == 0);
    });
    poll->start(50);
}

}  // namespace

void startSmokeTest(QApplication& app, MainWindow& window, const QString& file)
{
    std::printf("Piattaforma Qt: %s\n%s\n", qPrintable(QGuiApplication::platformName()),
                qPrintable(versionSummary()));
    std::fflush(stdout);
    if (file.isEmpty()) {
        QTimer::singleShot(0, &app, &QApplication::quit);
        return;
    }
    auto run = std::make_shared<SmokeRun>(app, window, file);
    QTimer::singleShot(0, &app, [run] {
        run->player->togglePlayPause();
        run->playStart.start();
    });
    QTimer::singleShot(kSmokePlayMilliseconds, &app, [run] {
        bool ok = checkPlayback(*run);
        ok = checkPlayerButtons(*run) && ok;
        ok = checkProxy(*run) && ok;
        if (core::isAudioOnly(TestAccess::currentFile(*run->window).toStdString())) {
            ok = checkAudioOnlyFile(*run) && ok;
            finishSmokeTest(*run, ok);
            return;
        }
        ok = buildSequence(*run) && ok;

        // Audio: un tratto silenziato a cavallo della prima giunzione, poi le forme d'onda pronte (controllati dopo
        // il salvataggio del progetto, che deve tenerli).
        const core::Sequence& sequence = run->window->project().sequence();
        const double muteFrom = std::max(0.0, sequence.startOf(1) - 0.1);
        TestAccess::muteRangeForTest(*run->window, muteFrom, muteFrom + 0.2);
        const auto muted = sequence.mutedRanges();
        QElapsedTimer waves;
        waves.start();
        while (TestAccess::pendingWaveforms(*run->window) > 0 && waves.elapsed() < 20000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        ok = checkUndoRedo(*run) && ok;
        ok = checkThumbnails(*run) && ok;
        ok = checkZoom(*run) && ok;
        ok = checkPanels(*run) && ok;
        ok = checkFileDrops(*run) && ok;
        ok = checkProjectSaveAndOpen(*run) && ok;
        std::printf("Audio: %d tratti senza audio (%.3f - %.3f s), forme d'onda %s\n", static_cast<int>(muted.size()),
                    muted.empty() ? 0.0 : muted.front().start, muted.empty() ? 0.0 : muted.front().end,
                    TestAccess::pendingWaveforms(*run->window) == 0 ? "pronte" : "NON pronte");
        std::fflush(stdout);
        ok = ok && muted.size() == 1 && TestAccess::pendingWaveforms(*run->window) == 0;

        ok = checkSequencePreview(*run) && ok;
        ok = checkGaps(*run) && ok;
        ok = checkPieceEdges(*run) && ok;
        ok = checkAudioTracks(*run) && ok;
        ok = checkTexts(*run) && ok;
        ok = checkTransitions(*run) && ok;
        ok = checkMixAndAudioOnly(*run) && ok;
        checkExportAndFinish(run, ok);
    });
}

}  // namespace editor::app
