#include "EditorApp/SelfTests.h"
#include "EditorApp/SequenceTimeline.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtGui/QMouseEvent>

#include <cmath>
#include <cstdio>

namespace editor::app {

// --layer-test: i pezzi di video sui livelli sopra V1 (V2, V3...) sulla linea del tempo: si vedono come blocchi, si prendono
// con il mouse (spostamento fra livelli, bordi), con Ctrl+clic non entrano nelle selezioni multiple e non hanno le
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

    // Ctrl+clic: il pezzo si sceglie da solo, niente selezione multipla.
    selectedText = 0;
    mouse(QEvent::MouseButtonPress, {grabX, yLow}, Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, {grabX, yLow}, Qt::ControlModifier);
    check(line.selectedObjects().empty() && line.selectedTextClip() == 11, "Ctrl+clic non lo mette in una selezione multipla");

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

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
