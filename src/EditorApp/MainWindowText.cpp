
#include "EditorApp/MainWindowShared.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorCore/ProjectFile.h"

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
    recordEdit(QStringLiteral("Aggiungi livello video"), before);
    statusBar()->showMessage(QStringLiteral("Aggiunto il livello %1: premi T per metterci una scritta, Maiusc+A un pezzo della Sorgente, al cursore.")
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
    const std::size_t clips = layer->clips.size() + layer->pieces.size();
    if (confirmRemovals && clips > 0) {
        // Togliere il livello cancella tutto quello che c'è sopra: si chiede prima.
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Togli il livello %1").arg(name),
                        QStringLiteral("Il livello %1 contiene %2 %3. Toglierlo li cancella.")
                            .arg(name)
                            .arg(clips)
                            .arg(clips == 1 ? QStringLiteral("oggetto (scritta o video)") : QStringLiteral("oggetti (scritte o video)")),
                        QMessageBox::NoButton, this);
        box.setInformativeText(QStringLiteral("Con Ctrl+Z si rimette com'era."));
        QPushButton* remove = box.addButton(QStringLiteral("Togli il livello"), QMessageBox::DestructiveRole);
        QPushButton* keep = box.addButton(QStringLiteral("Annulla"), QMessageBox::RejectRole);
        box.setDefaultButton(keep);
        box.setEscapeButton(keep);
        box.exec();
        if (box.clickedButton() != remove)
            return false;
    }
    const core::Project before = project_;
    project_.textTracks().removeLayer(layerId);
    recordEdit(QStringLiteral("Togli livello video"), before);
    statusBar()->showMessage(clips == 0 ? QStringLiteral("Tolto il livello %1.").arg(name)
                                        : QStringLiteral("Tolto il livello %1 con %2 oggetti (Ctrl+Z lo rimette).").arg(name).arg(clips),
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
    QSettings s = settings();
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
    QSettings s = settings();
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
    propertiesDock_->show();
    propertiesDock_->raise();
    refreshTextPanel();
    refreshPropertiesPanel();
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
    // La trasformazione e l'animazione non sono dell'editor del testo (le cambia il pannello Proprietà): non contano nel confronto.
    core::TextClip comparable = edited;
    comparable.transform = current->transform;
    comparable.animation = current->animation;
    if (*current == comparable && !stylesChanged)
        return true;  // niente è cambiato
    // Scrivere una frase nel pannello è una sola modifica: se l'ultima voce di Annulla è la precedente modifica di questa
    // scritta (e dopo non è successo altro), si continua quella.
    const bool onlyStyles = *current == comparable;  // (qui sono cambiati solo gli stili con nome)
    const std::string key = merge ? std::to_string(edited.id) : std::string();
    std::optional<core::Project> before;
    if (!continuesEdit(textMerge_, key, QStringLiteral("Modifica scritta")))
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
    finishMergedEdit(textMerge_, key, onlyStyles ? QStringLiteral("Cambia stili delle scritte") : QStringLiteral("Modifica scritta"),
                     before);
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

void MainWindow::liftFragmentToLayer(int fragmentId, int layerId, double position)
{
    const core::Project before = project_;
    int audio = 0;
    const int piece = project_.liftToLayer(fragmentId, layerId, position, &audio);
    if (piece == 0) {
        statusBar()->showMessage(QStringLiteral("Qui il pezzo non può passare sul livello: non c'è posto."), 6000);
        return;
    }
    timeline_->setSelectedTextClip(piece);
    recordEdit(QStringLiteral("Sposta il video su un livello"), before);
    statusBar()->showMessage(audio != 0 ? QStringLiteral("Il video è passato sul livello, con il suo audio in gruppo: in V1 resta uno spazio vuoto.")
                                        : QStringLiteral("Il video è passato sul livello: in V1 resta uno spazio vuoto."),
                             8000);
}

void MainWindow::lowerPieceToV1(int pieceId, double position)
{
    const core::Project before = project_;
    const int fragment = project_.lowerToV1(pieceId, position, pushPieces_);
    if (fragment == 0)
        return;
    timeline_->setSelected(fragment);
    recordEdit(QStringLiteral("Sposta il video su V1"), before);
    statusBar()->showMessage(QStringLiteral("Il video è tornato su V1."), 6000);
}

bool MainWindow::moveTextClip(int clipId, int layerId, double position)
{
    const bool isPiece = project_.textTracks().findPiece(clipId) != nullptr;  // un pezzo di video su un livello
    if (!isPiece && project_.textTracks().find(clipId) == nullptr)
        return false;
    const core::Project before = project_;
    const core::Project::MoveResult moved = project_.moveObject(
        {isPiece ? core::ObjectRef::Kind::LayerPiece : core::ObjectRef::Kind::TextClip, clipId}, layerId, position, pushPieces_);
    if (!moved.moved)
        return false;
    recordEdit(moved.group ? QStringLiteral("Sposta gruppo")
                           : isPiece ? QStringLiteral("Sposta pezzo del livello") : QStringLiteral("Sposta scritta"),
               before);
    return true;
}

bool MainWindow::trimTextClip(int clipId, bool atStart, double value)
{
    const core::Project before = project_;
    core::TextTracks& text = project_.textTracks();
    if (const core::LayerPiece* piece = text.findPiece(clipId)) {
        // Il bordo si sposta anche nel file, di `delta * velocità` (con l'inverso nel verso opposto): a sinistra cambiano la
        // posizione e un bordo del file, l'altro resta dov'è.
        const double delta = (value - (atStart ? piece->position : piece->endPosition())) * piece->speed;
        const double fileDelta = piece->reverse ? -delta : delta;
        const bool fileStartMoves = atStart != piece->reverse;  // il bordo del file che si sposta
        const double newStart = fileStartMoves ? piece->start + fileDelta : piece->start;
        const double newEnd = fileStartMoves ? piece->end : piece->end + fileDelta;
        const bool done = text.trimPiece(clipId, atStart ? value : piece->position, newStart, newEnd);
        if (!done)
            return false;
        recordEdit(atStart ? QStringLiteral("Sposta inizio del pezzo") : QStringLiteral("Sposta fine del pezzo"), before);
        return true;
    }
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
