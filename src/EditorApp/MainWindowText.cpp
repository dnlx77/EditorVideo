#include "EditorApp/MainWindow.h"

#include "EditorApp/MainWindowShared.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorCore/ProjectFile.h"
#include "EditorApp/UiText.h"

#include <QtCore/QSettings>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>

#include <algorithm>
#include <optional>

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

QSize MainWindow::frameSize() const
{
    const core::StreamSignature* video = project_.format().video();
    if (video != nullptr && video->width > 0 && video->height > 0)
        return {video->width, video->height};
    return {1920, 1080};
}

namespace {

const char* const kGlobalStylesKey = "scritte/stili";

}  // namespace

std::vector<core::NamedTextStyle> MainWindow::availableTextStyles() const
{
    // Quelli del progetto, poi quelli dell'elenco globale che il progetto non ha (stesso nome = vale quello del progetto).
    std::vector<core::NamedTextStyle> styles = project_.textStyles();
    QSettings s = detail::settings();
    for (core::NamedTextStyle& global : core::textStylesFromText(s.value(QString::fromLatin1(kGlobalStylesKey)).toString().toStdString())) {
        const bool known = std::any_of(styles.begin(), styles.end(), [&](const core::NamedTextStyle& n) { return n.name == global.name; });
        if (!known)
            styles.push_back(std::move(global));
    }
    return styles;
}

void MainWindow::keepTextStyles(const std::vector<core::NamedTextStyle>& styles)
{
    // Nel progetto (e quindi con Annulla) e nell'elenco globale, per gli altri progetti.
    project_.textStyles() = styles;
    QSettings s = detail::settings();
    s.setValue(QString::fromLatin1(kGlobalStylesKey), QString::fromStdString(core::textStylesToText(styles)));
}

int MainWindow::addTextClip(bool withEditor)
{
    core::TextTracks& text = project_.textTracks();
    const double at = std::max(0.0, program_->position());
    core::TextClip clip;
    clip.position = at;
    clip.length = kNewTextLength;
    clip.text = kNewTextDefault;
    const core::Project before = project_;
    // Il livello scelto se lì c'è posto, altrimenti il primo dove ci sta; se non ci sta da nessuna parte, un livello nuovo.
    std::vector<int> candidates;
    if (text.layer(timeline_->selectedTextLayer()) != nullptr)
        candidates.push_back(timeline_->selectedTextLayer());
    for (const core::TextLayer& layer : text.layers())
        candidates.push_back(layer.id);
    int layerId = 0;
    for (const int candidate : candidates) {
        if (text.canPlace(candidate, at, clip.length)) {
            layerId = candidate;
            break;
        }
    }
    if (layerId == 0)
        layerId = text.addLayer();
    const int id = text.add(layerId, clip);
    if (id == 0) {
        statusBar()->showMessage(QStringLiteral("Non si può mettere una scritta qui."), 6000);
        return 0;
    }
    timeline_->setSelectedTextClip(id);
    recordEdit(QStringLiteral("Aggiungi scritta"), before);
    statusBar()->showMessage(QStringLiteral("Aggiunta una scritta di %1 secondi sul livello %2, da %3.")
                                 .arg(clip.length)
                                 .arg(qs(text.layerName(layerId)), timeText(at)),
                             8000);
    if (withEditor)
        showTextPanel(id, true);  // il testo è già selezionato: basta scrivere
    return id;
}

bool MainWindow::editTextClip(int clipId)
{
    if (project_.textTracks().find(clipId) == nullptr) {
        statusBar()->showMessage(QStringLiteral("Scegli prima una scritta sulla linea del tempo."), 6000);
        return false;
    }
    timeline_->setSelectedTextClip(clipId);
    showTextPanel(clipId, true);
    return true;
}

void MainWindow::showTextPanel(int clipId, bool focusText)
{
    if (timeline_->selectedTextClip() != clipId)
        timeline_->setSelectedTextClip(clipId);
    if (fullScreenDock_ != nullptr)
        exitFullScreen();
    sourceDock_->show();
    sourceDock_->raise();
    sourceTabs_->setCurrentWidget(textPanel_);
    refreshTextPanel();
    if (focusText)
        textPanel_->focusText();
}

void MainWindow::refreshTextPanel()
{
    if (textPanel_ == nullptr)
        return;
    const core::TextClip* clip = project_.textTracks().find(timeline_->selectedTextClip());
    if (clip == nullptr)
        textPanel_->setClip(nullptr, {});
    else
        textPanel_->setClip(clip, availableTextStyles());
}

bool MainWindow::applyTextEdit(const core::TextClip& edited, const std::vector<core::NamedTextStyle>* styles, bool merge)
{
    const core::TextClip* current = project_.textTracks().find(edited.id);
    if (current == nullptr)
        return false;
    const bool stylesChanged = styles != nullptr && *styles != availableTextStyles();
    if (*current == edited && !stylesChanged)
        return true;  // niente è cambiato
    // Scrivere una frase nel pannello è una sola modifica: se l'ultima voce di Annulla è la precedente modifica di questa
    // scritta (e dopo non è successo altro), si continua quella.
    const bool onlyStyles = *current == edited;  // (qui sono cambiati solo gli stili con nome)
    const bool continues = merge && textMergeClip_ == edited.id && textMergeCount_ == history_.undoCount() && history_.canUndo()
                        && history_.undoLabel() == "Modifica scritta" && !history_.canRedo();
    std::optional<core::Project> before;
    if (!continues)
        before = project_;
    if (stylesChanged)
        keepTextStyles(*styles);
    core::TextTracks& text = project_.textTracks();
    if (std::abs(edited.length - current->length) > 0.0005 && !text.setLength(edited.id, edited.length)) {
        statusBar()->showMessage(QStringLiteral("La durata scelta non ci sta: la scritta urterebbe la successiva sullo stesso livello."), 8000);
        return false;
    }
    text.setText(edited.id, edited.text);
    text.setStyle(edited.id, edited.style);
    text.setFades(edited.id, edited.fadeIn, edited.fadeOut);
    timeline_->setSelectedTextClip(edited.id);
    if (continues && history_.amend())
        sequenceChanged();
    else
        recordEdit(onlyStyles ? QStringLiteral("Cambia stili delle scritte") : QStringLiteral("Modifica scritta"), before ? *before : project_);
    textMergeClip_ = merge ? edited.id : 0;
    textMergeCount_ = history_.undoCount();
    return true;
}

bool MainWindow::moveTextOnScreen(int clipId, double centerX, double centerY)
{
    const core::TextClip* current = project_.textTracks().find(clipId);
    if (current == nullptr)
        return false;
    if (std::abs(current->style.centerX - centerX) < 0.001 && std::abs(current->style.centerY - centerY) < 0.001)
        return true;
    const core::Project before = project_;
    core::TextStyle style = current->style;
    style.centerX = centerX;
    style.centerY = centerY;
    project_.textTracks().setStyle(clipId, style);
    recordEdit(QStringLiteral("Sposta scritta sullo schermo"), before);
    return true;
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

bool MainWindow::fadeTextClip(int clipId, double fadeIn, double fadeOut)
{
    const core::Project before = project_;
    if (!project_.textTracks().setFades(clipId, fadeIn, fadeOut))
        return false;
    recordEdit(QStringLiteral("Cambia dissolvenza della scritta"), before);
    return true;
}

}  // namespace editor::app
