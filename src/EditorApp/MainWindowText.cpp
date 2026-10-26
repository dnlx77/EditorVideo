#include "EditorApp/MainWindow.h"

#include "EditorApp/MainWindowShared.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/UiText.h"

#include <QtWidgets/QStatusBar>

#include <algorithm>

namespace editor::app {

// Le scritte (step T3): i livelli di scritte sopra la corsia video, i loro comandi e le modifiche con Annulla/Ripeti.

namespace {

constexpr double kNewTextLength = 5.0;  // una scritta nuova dura 5 secondi
const char* const kNewTextDefault = "Scritta";

}  // namespace

int MainWindow::addTextLayer()
{
    const core::Project before = project_;
    const int id = project_.textTracks().addLayer();
    timeline_->setSelectedTextLayer(id);
    recordEdit(QStringLiteral("Aggiungi livello di scritte"), before);
    statusBar()->showMessage(QStringLiteral("Aggiunto il livello di scritte %1: premi T per metterci una scritta al cursore.")
                                 .arg(qs(project_.textTracks().layerName(id))),
                             8000);
    return id;
}

bool MainWindow::removeTextLayer(int layerId)
{
    const core::TextLayer* layer = project_.textTracks().layer(layerId);
    if (layer == nullptr)
        return false;
    const QString name = qs(project_.textTracks().layerName(layerId));
    const std::size_t clips = layer->clips.size();
    const core::Project before = project_;
    project_.textTracks().removeLayer(layerId);
    recordEdit(QStringLiteral("Togli livello di scritte"), before);
    statusBar()->showMessage(clips == 0 ? QStringLiteral("Tolto il livello di scritte %1.").arg(name)
                                        : QStringLiteral("Tolto il livello di scritte %1 con %2 scritte (Ctrl+Z lo rimette).").arg(name).arg(clips),
                             8000);
    return true;
}

void MainWindow::removeSelectedTextLayer()
{
    const core::TextTracks& text = project_.textTracks();
    if (text.layers().empty())
        return;
    // Quello scelto sulla linea del tempo, altrimenti l'ultimo (il più alto).
    const int selected = timeline_->selectedTextLayer();
    removeTextLayer(text.layer(selected) != nullptr ? selected : text.layers().back().id);
}

int MainWindow::addTextClip()
{
    core::TextTracks& text = project_.textTracks();
    const double at = std::max(0.0, program_->position());
    const core::Project before = project_;
    // Il livello scelto se lì c'è posto, altrimenti il primo dove ci sta; se non ci sta da nessuna parte, un livello nuovo.
    std::vector<int> candidates;
    if (text.layer(timeline_->selectedTextLayer()) != nullptr)
        candidates.push_back(timeline_->selectedTextLayer());
    for (const core::TextLayer& layer : text.layers())
        candidates.push_back(layer.id);
    int layerId = 0;
    for (const int candidate : candidates) {
        if (text.canPlace(candidate, at, kNewTextLength)) {
            layerId = candidate;
            break;
        }
    }
    if (layerId == 0)
        layerId = text.addLayer();
    core::TextClip clip;
    clip.position = at;
    clip.length = kNewTextLength;
    clip.text = kNewTextDefault;
    const int id = text.add(layerId, clip);
    if (id == 0) {
        statusBar()->showMessage(QStringLiteral("Non si può mettere una scritta qui."), 6000);
        return 0;
    }
    timeline_->setSelectedTextClip(id);
    recordEdit(QStringLiteral("Aggiungi scritta"), before);
    statusBar()->showMessage(QStringLiteral("Aggiunta una scritta di %1 secondi sul livello %2, da %3.")
                                 .arg(kNewTextLength)
                                 .arg(qs(text.layerName(layerId)), timeText(at)),
                             8000);
    return id;
}

bool MainWindow::moveTextClip(int clipId, int layerId, double position)
{
    if (project_.textTracks().find(clipId) == nullptr)
        return false;
    const core::Project before = project_;
    const core::Project::MoveResult moved =
        project_.moveObject({core::ObjectRef::Kind::TextClip, clipId}, layerId, position, pushPieces_);
    if (!moved.moved)
        return false;
    recordEdit(moved.group ? QStringLiteral("Sposta gruppo") : QStringLiteral("Sposta scritta"), before);
    return true;
}

bool MainWindow::trimTextClip(int clipId, bool atStart, double value)
{
    const core::Project before = project_;
    core::TextTracks& text = project_.textTracks();
    if (!(atStart ? text.setStart(clipId, value) : text.setEnd(clipId, value)))
        return false;
    recordEdit(atStart ? QStringLiteral("Sposta inizio della scritta") : QStringLiteral("Sposta fine della scritta"), before);
    return true;
}

}  // namespace editor::app
