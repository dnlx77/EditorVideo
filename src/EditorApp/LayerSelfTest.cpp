#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SelfTests.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtGui/QMouseEvent>
#include <QtCore/QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace editor::app {

// --layer-test: i pezzi di video sui livelli sopra V1 (V2, V3...) sulla linea del tempo: si vedono come blocchi, si prendono
// con il mouse (spostamento fra livelli, bordi), con Ctrl+clic entrano nelle selezioni multiple e non hanno le
// dissolvenze delle scritte. Codice 0 se tutto è giusto, 3 se un controllo non riesce.
int runLayerTest(const QStringList&, qsizetype)
{
    SelfCheck check;
    std::printf("Prova dei pezzi di video sui livelli\n");

    SequenceTimeline line;
    line.resize(1000, 380);
    line.show();
    QCoreApplication::processEvents();
    SequenceTimeline::Block video;
    video.fragmentId = 1;
    video.clipId = 1;
    video.label = "v1.mp4";
    video.length = 12.0;
    line.setBlocks({video});

    SequenceTimeline::TextLane low;
    low.layerId = 1;
    low.name = "V2";
    low.clips.push_back({10, "Titolo", 1.0, 2.0, 0.0, 0.0});
    SequenceTimeline::TextBlock piece{11, "gita.mp4", 4.0, 4.0, 0.0, 0.0};
    piece.piece = true;
    low.clips.push_back(piece);
    SequenceTimeline::TextLane high;
    high.layerId = 2;
    high.name = "V3";
    line.setTextLanes({low, high});
    line.fitAll();
    QCoreApplication::processEvents();

    int movedClip = 0;
    int movedLayer = 0;
    double movedTo = -1.0;
    line.textFreePosition = [](int, int, double desired) { return desired; };
    line.onTextClipMoveTo = [&](int clip, int layer, double position) {
        movedClip = clip;
        movedLayer = layer;
        movedTo = position;
    };
    int trimmedClip = 0;
    bool trimmedStart = false;
    double trimmedValue = -1.0;
    line.textTrimLimit = [](int, bool, double desired) { return desired; };
    line.onTextTrim = [&](int clip, bool atStart, double value) {
        trimmedClip = clip;
        trimmedStart = atStart;
        trimmedValue = value;
    };
    bool fadeChanged = false;
    line.onTextFades = [&](int, double, double) { fadeChanged = true; };
    int selectedText = 0;
    line.onTextClipSelected = [&](int id) { selectedText = id; };

    const auto mouse = [&](QEvent::Type type, QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent e(type, p, line.mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
        QCoreApplication::sendEvent(&line, &e);
    };
    const double yLow = line.textLaneCenter(0);
    const double yHigh = line.textLaneCenter(1);
    const double perSecond = line.xForSequenceTime(1.0) - line.xForSequenceTime(0.0);
    check(yHigh < yLow, "il livello più alto sta più in alto");

    // Si prende al centro, si porta sul livello sopra, 1 secondo più avanti.
    const double grabX = line.xForSequenceTime(6.0);
    mouse(QEvent::MouseButtonPress, {grabX, yLow});
    check(selectedText == 11 && line.selectedTextClip() == 11, "un clic sul pezzo lo sceglie");
    mouse(QEvent::MouseMove, {grabX + perSecond / 2, (yLow + yHigh) / 2});
    mouse(QEvent::MouseMove, {grabX + perSecond, yHigh});
    mouse(QEvent::MouseButtonRelease, {grabX + perSecond, yHigh});
    check(movedClip == 11 && movedLayer == 2 && std::abs(movedTo - 5.0) < 0.1, "trascinato sul livello V3, da 5 secondi");

    // Ctrl+clic: il pezzo entra nella selezione multipla insieme a una scritta.
    const double titleX = line.xForSequenceTime(2.0);
    mouse(QEvent::MouseButtonPress, {titleX, yLow}, Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, {titleX, yLow}, Qt::ControlModifier);
    const auto picked = line.selectedObjects();
    check(picked.size() == 2
              && std::find(picked.begin(), picked.end(), core::ObjectRef{core::ObjectRef::Kind::LayerPiece, 11}) != picked.end()
              && std::find(picked.begin(), picked.end(), core::ObjectRef{core::ObjectRef::Kind::TextClip, 10}) != picked.end(),
          "Ctrl+clic mette il pezzo e la scritta nella selezione multipla");
    // Un clic semplice sul pezzo torna alla selezione singola.
    mouse(QEvent::MouseButtonPress, {grabX, yLow});
    mouse(QEvent::MouseButtonRelease, {grabX, yLow});
    check(line.selectedObjects().empty() && line.selectedTextClip() == 11, "un clic semplice torna al pezzo da solo");

    // Il bordo sinistro, 1 secondo più avanti: si accorcia l'inizio.
    const double edgeX = line.xForSequenceTime(4.0) + 1;
    mouse(QEvent::MouseButtonPress, {edgeX, yLow});
    mouse(QEvent::MouseMove, {edgeX + perSecond / 2, yLow});
    mouse(QEvent::MouseMove, {edgeX + perSecond, yLow});
    mouse(QEvent::MouseButtonRelease, {edgeX + perSecond, yLow});
    check(trimmedClip == 11 && trimmedStart && std::abs(trimmedValue - 5.0) < 0.1, "il bordo sinistro si sposta (inizio a 5 s)");

    // In alto a sinistra, dove nelle scritte c'è la maniglia della dissolvenza: sul pezzo non c'è.
    mouse(QEvent::MouseButtonPress, {line.xForSequenceTime(4.0) + 8, yLow - 6});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(4.0) + 40, yLow - 6});
    mouse(QEvent::MouseButtonRelease, {line.xForSequenceTime(4.0) + 40, yLow - 6});
    check(!fadeChanged, "un pezzo non ha le maniglie delle dissolvenze");

    // Un video di V1 trascinato su un livello: arriva la richiesta con il livello e il punto.
    int liftedFragment = 0;
    int liftedLayer = 0;
    double liftedAt = -1.0;
    line.onPieceToLayer = [&](int fragment, int layer, double position) {
        liftedFragment = fragment;
        liftedLayer = layer;
        liftedAt = position;
    };
    int loweredPiece = 0;
    double loweredAt = -1.0;
    line.onLayerPieceToV1 = [&](int piece, double position) {
        loweredPiece = piece;
        loweredAt = position;
    };
    const double yVideo = line.videoLaneCenter();
    const double v1X = line.xForSequenceTime(3.0);
    mouse(QEvent::MouseButtonPress, {v1X, yVideo});
    mouse(QEvent::MouseMove, {v1X + perSecond / 2, (yVideo + yHigh) / 2});
    mouse(QEvent::MouseMove, {v1X + perSecond, yHigh});
    mouse(QEvent::MouseButtonRelease, {v1X + perSecond, yHigh});
    check(liftedFragment == 1 && liftedLayer == 2 && std::abs(liftedAt - 1.0) < 0.15, "un video di V1 lasciato sul livello V3 diventa un pezzo del livello (da 1 s)");
    // Lasciato ancora su V1: è un normale spostamento, non passa sul livello.
    liftedFragment = 0;
    mouse(QEvent::MouseButtonPress, {v1X, yVideo});
    mouse(QEvent::MouseMove, {v1X + perSecond / 2, yVideo});
    mouse(QEvent::MouseMove, {v1X + perSecond, yVideo});
    mouse(QEvent::MouseButtonRelease, {v1X + perSecond, yVideo});
    check(liftedFragment == 0, "lasciato su V1 non passa su un livello");
    // Un pezzo di livello lasciato sulla corsia di V1 torna nella sequenza; una scritta no.
    mouse(QEvent::MouseButtonPress, {grabX, yLow});
    mouse(QEvent::MouseMove, {grabX + perSecond / 2, (yLow + yVideo) / 2});
    mouse(QEvent::MouseMove, {grabX + perSecond, yVideo});
    mouse(QEvent::MouseButtonRelease, {grabX + perSecond, yVideo});
    check(loweredPiece == 11 && std::abs(loweredAt - 5.0) < 0.15, "un pezzo di livello lasciato su V1 torna nella sequenza (da 5 s)");
    loweredPiece = 0;
    movedClip = 0;
    mouse(QEvent::MouseButtonPress, {line.xForSequenceTime(2.0), yLow});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(2.0) + perSecond / 2, (yLow + yVideo) / 2});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(2.0) + perSecond, yVideo});
    mouse(QEvent::MouseButtonRelease, {line.xForSequenceTime(2.0) + perSecond, yVideo});
    check(loweredPiece == 0, "una scritta lasciata su V1 non è un pezzo che torna nella sequenza");
    check(movedClip == 10 && movedLayer == 0, "una scritta lasciata su V1 chiede di spostarsi su V1 (livello 0)");

    // Le scritte che stanno su V1, accanto al video (che dura 12 s): si prendono e si spostano lungo V1, e un clic fuori da
    // loro è ancora sul video.
    SequenceTimeline::TextBlock onV1{20, "Su V1", 13.0, 2.0, 0.0, 0.0};
    line.setBaseTexts({onV1});
    QCoreApplication::processEvents();
    selectedText = 0;
    mouse(QEvent::MouseButtonPress, {line.xForSequenceTime(14.0), yVideo});
    mouse(QEvent::MouseButtonRelease, {line.xForSequenceTime(14.0), yVideo});
    check(selectedText == 20, "un clic su una scritta di V1 la sceglie");
    movedClip = 0;
    mouse(QEvent::MouseButtonPress, {line.xForSequenceTime(14.0), yVideo});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(14.0) + perSecond / 2, yVideo});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(14.0) + perSecond, yVideo});
    mouse(QEvent::MouseButtonRelease, {line.xForSequenceTime(14.0) + perSecond, yVideo});
    check(movedClip == 20 && movedLayer == 0 && std::abs(movedTo - 14.0) < 0.5, "una scritta di V1 si sposta lungo V1 (a 14 s)");
    selectedText = 0;
    mouse(QEvent::MouseButtonPress, {line.xForSequenceTime(5.0), yVideo});
    mouse(QEvent::MouseButtonRelease, {line.xForSequenceTime(5.0), yVideo});
    check(selectedText == 0 && line.selectedTextClip() == 0, "un clic sul video di V1, lontano dalle scritte, non sceglie scritte");
    // Dalla corsia di V1 una scritta sale su un livello.
    movedClip = 0;
    mouse(QEvent::MouseButtonPress, {line.xForSequenceTime(14.0), yVideo});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(14.0), (yVideo + yLow) / 2});
    mouse(QEvent::MouseMove, {line.xForSequenceTime(14.0), yLow});
    mouse(QEvent::MouseButtonRelease, {line.xForSequenceTime(14.0), yLow});
    check(movedClip == 20 && movedLayer == 1, "una scritta di V1 trascinata su V2 chiede il livello 1");

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}


// --layer-monitor-test <video>: il monitor del Montaggio mostra i pezzi dei livelli sopra V1: nessuno prima che inizino e dopo
// che finiscono, uno mentre ci si è dentro (da fermo e in riproduzione), due su due livelli.
int runLayerMonitorTest(const QStringList& args, qsizetype at)
{
    SelfCheck check;
    std::printf("Prova dei livelli nel monitor\n");
    if (at + 1 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --layer-monitor-test <video>\n");
        return 1;
    }
    namespace core = editor::core;
    const std::string path = args[at + 1].toStdString();
    core::Project project;
    const core::MediaInfo info = core::probeFile(path);
    project.setFormat(core::formatOf(core::readMediaSignature(path)));
    const int clip = project.add(path);
    project.setAnalysis(clip, info.keyframeTimes, info.durationSeconds);
    const double length = std::min(info.durationSeconds, 3.0);
    project.sequence().insert(-1, clip, 0.0, length);
    const int low = project.textTracks().addLayer();
    const int high = project.textTracks().addLayer();
    core::LayerPiece piece;
    piece.clipId = clip;
    piece.start = 0.0;
    piece.end = 1.0;
    piece.position = 1.0;
    piece.transform.scaleX = 0.5;
    piece.transform.scaleY = 0.5;
    check(project.textTracks().addPiece(low, piece) != 0, "il primo pezzo del livello sta sul livello V2");
    piece.position = 1.5;
    check(project.textTracks().addPiece(high, piece) != 0, "il secondo pezzo sta su V3");

    ProgramMonitor monitor(project);
    monitor.resize(900, 600);
    monitor.show();
    monitor.sequenceChanged();
    QCoreApplication::processEvents();

    monitor.seekTo(0.3);
    QCoreApplication::processEvents();
    check(monitor.layerFramesShown() == 0, "prima dei pezzi dei livelli non se ne vede nessuno");
    monitor.seekTo(1.2);
    QCoreApplication::processEvents();
    check(monitor.layerFramesShown() == 1, "dentro il primo pezzo se ne vede uno");
    monitor.seekTo(1.7);
    QCoreApplication::processEvents();
    check(monitor.layerFramesShown() == 2, "dove i due pezzi si sovrappongono sui due livelli se ne vedono due");
    monitor.seekTo(2.7);
    QCoreApplication::processEvents();
    check(monitor.layerFramesShown() == 0, "dopo la fine dei pezzi non se ne vede nessuno");

    // In riproduzione da prima del pezzo: arrivato dentro, il fotogramma arriva.
    monitor.seekTo(0.7);
    monitor.setPlaying(true);
    int best = 0;
    for (int i = 0; i < 120 && best < 1; ++i) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
        if (monitor.position() > 1.1 && monitor.position() < 1.9)
            best = std::max(best, monitor.layerFramesShown());
    }
    monitor.setPlaying(false);
    check(best >= 1, "in riproduzione il fotogramma del livello arriva quando il pezzo inizia");

    // Le maniglie sul pezzo di un livello scelto: compaiono, il gesto cambia la trasformazione di quel pezzo.
    {
        const int pieceId = project.textTracks().layers().front().pieces.front().id;
        int selected = 0;
        monitor.selectedLayerPiece = [&] { return selected; };
        int editedId = 0;
        core::Transform edited;
        monitor.onLayerTransformEdited = [&](int id, const core::Transform& t) {
            editedId = id;
            edited = t;
        };
        monitor.setTransformMode(true);
        monitor.seekTo(1.2);
        QCoreApplication::processEvents();
        check(!monitor.manipulatorsVisible() || selected == 0, "con un pezzo di V1 sotto e nessun livello scelto vale V1");
        selected = pieceId;
        monitor.refreshManipulators();
        check(monitor.manipulatorsVisible(), "scelto il pezzo di un livello le maniglie ci sono");
        PlayerWidget* player = nullptr;
        for (QWidget* w : monitor.findChildren<QWidget*>()) {
            player = dynamic_cast<PlayerWidget*>(w);
            if (player != nullptr)
                break;
        }
        check(player != nullptr, "il monitor ha il lettore");
        if (player != nullptr) {
            const QRectF canvas = player->imageRect();
            const double projW = project.format().video()->width;
            const double projH = project.format().video()->height;
            const double scale = canvas.width() / projW;
            QWidget* view = player->childAt(canvas.center().toPoint());
            check(view != nullptr, "c'è un widget sotto l'immagine");
            if (view != nullptr) {
                const auto at = [&](double x, double y) { return QPointF(canvas.left() + x * scale, canvas.top() + y * scale); };
                const auto send = [&](QEvent::Type type, QPointF p) {
                    QMouseEvent e(type, p, view->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(view, &e);
                };
                send(QEvent::MouseButtonPress, at(projW / 2.0, projH / 2.0));
                send(QEvent::MouseMove, at(projW / 2.0 + 30, projH / 2.0 + 10));
                check(editedId == 0, "durante il gesto sul livello non arriva ancora nessuna modifica");
                send(QEvent::MouseMove, at(projW / 2.0 + 60, projH / 2.0 + 20));
                send(QEvent::MouseButtonRelease, at(projW / 2.0 + 60, projH / 2.0 + 20));
                check(editedId == pieceId, "al rilascio la modifica è per il pezzo del livello");
                check(std::abs(edited.posX - 60) < 1.0 && std::abs(edited.posY - 20) < 1.0, "la posizione cambia di quanto trascinato");
            }
        }
        monitor.setTransformMode(false);
        selected = 0;
    }

    // Un pezzo di livello oltre la fine di V1: il monitor ci arriva, lo mostra e si ferma solo alla sua fine.
    piece.position = length + 0.5;
    check(project.textTracks().addPiece(low, piece) != 0, "un pezzo dopo la fine del video principale");
    monitor.sequenceChanged();
    const double programEnd = length + 1.5;
    check(std::abs(monitor.programEnd() - programEnd) < 1e-6, "la fine del programma è la fine del pezzo oltre V1");
    monitor.seekTo(length + 1.0);
    QCoreApplication::processEvents();
    check(std::abs(monitor.position() - (length + 1.0)) < 0.05, "ci si può posizionare dopo la fine di V1");
    check(monitor.layerFramesShown() == 1, "dopo la fine di V1 si vede il pezzo del livello");
    monitor.seekTo(length - 0.3);
    monitor.setPlaying(true);
    double reached = 0.0;
    for (int i = 0; i < 400 && monitor.isPlaying(); ++i) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
        reached = std::max(reached, monitor.position());
    }
    check(reached > length + 1.2, "la riproduzione continua oltre la fine di V1");
    check(!monitor.isPlaying(), "la riproduzione si ferma alla fine del programma");

    // Senza pezzi su V1, solo con una scritta su un livello: la riproduzione parte lo stesso e arriva alla fine della scritta.
    {
        core::Project onlyText;
        const int layer = onlyText.textTracks().addLayer();
        core::TextClip text;
        text.position = 0.5;
        text.length = 1.0;
        text.text = "Solo scritta";
        check(onlyText.textTracks().add(layer, text) != 0, "una scritta su un livello senza video su V1");
        ProgramMonitor bare(onlyText);
        bare.resize(640, 360);
        bare.show();
        bare.sequenceChanged();
        QCoreApplication::processEvents();
        check(std::abs(bare.programEnd() - 1.5) < 1e-6, "la fine del programma è la fine della scritta");
        bare.seekTo(1.0);
        QCoreApplication::processEvents();
        check(std::abs(bare.position() - 1.0) < 0.05, "ci si posiziona dentro la scritta senza video");
        bare.seekTo(0.0);
        bare.setPlaying(true);
        check(bare.isPlaying(), "la riproduzione parte anche senza video su V1");
        double got = 0.0;
        for (int i = 0; i < 300 && bare.isPlaying(); ++i) {
            QThread::msleep(20);
            QCoreApplication::processEvents();
            got = std::max(got, bare.position());
        }
        check(got > 1.3 && !bare.isPlaying(), "arriva in fondo alla scritta e si ferma");
    }

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
