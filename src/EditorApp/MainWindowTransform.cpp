#include "EditorApp/MainWindow.h"

#include "EditorApp/MainWindowShared.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/PropertiesPanel.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorCore/Compatibility.h"

#include <QtWidgets/QStatusBar>

#include <algorithm>
#include <cmath>

namespace editor::app {
using namespace detail;

namespace {

// I pezzi video fra gli oggetti scelti, nell'ordine di scelta.
std::vector<int> selectedPieceIds(const std::vector<core::ObjectRef>& objects)
{
    std::vector<int> ids;
    for (const core::ObjectRef& o : objects) {
        if (o.kind == core::ObjectRef::Kind::Piece && std::find(ids.begin(), ids.end(), o.id) == ids.end())
            ids.push_back(o.id);
    }
    return ids;
}

}  // namespace

void MainWindow::refreshPropertiesPanel()
{
    if (propertiesPanel_ == nullptr || timeline_ == nullptr)
        return;
    const std::vector<core::ObjectRef> objects = timeline_->allSelectedObjects();
    if (objects.size() == 1 && objects.front().kind == core::ObjectRef::Kind::TextClip
        && project_.textTracks().find(objects.front().id) != nullptr) {
        propertiesPanel_->showText();  // (refreshTextPanel ne mette i valori)
        return;
    }
    const std::vector<int> pieces = selectedPieceIds(objects);
    for (int id : pieces) {
        if (const core::Fragment* f = project_.sequence().find(id)) {
            propertiesPanel_->showTransform(f->transform, static_cast<int>(pieces.size()));
            return;
        }
    }
    QString message = QStringLiteral("Scegli un pezzo video sulla linea del tempo per cambiarne scala, posizione, rotazione e ritaglio.");
    if (!objects.empty()) {
        const core::ObjectRef::Kind kind = objects.front().kind;
        if (kind == core::ObjectRef::Kind::AudioClip || kind == core::ObjectRef::Kind::PieceAudio)
            message = QStringLiteral("Per l'audio il livello si cambia con la linea sulla clip e con i comandi della corsia.");
    } else if (timeline_->selectedTransition() != 0) {
        message = QStringLiteral("Le proprietà della transizione si cambiano con doppio clic sul blocco.");
    }
    propertiesPanel_->showNothing(message);
}

bool MainWindow::applyTransformEdit(const std::function<void(core::Transform&)>& edit, const std::string& field)
{
    const std::vector<int> ids = selectedPieceIds(timeline_->allSelectedObjects());
    if (ids.empty())
        return false;
    const core::Project before = project_;
    std::string key = field;
    int changed = 0;
    for (int id : ids) {
        const core::Fragment* f = project_.sequence().find(id);
        if (f == nullptr)
            continue;
        core::Transform t = f->transform;
        edit(t);
        changed += project_.setTransform({id}, t);
        key += ':' + std::to_string(id);
    }
    if (changed == 0)
        return false;
    // Un cursore o una casella girati più volte di fila sullo stesso campo sono una sola voce di Annulla.
    const bool continues = transformMergeKey_ == key && transformMergeCount_ == history_.undoCount() && history_.canUndo()
                        && history_.undoLabel() == "Cambia trasformazione" && !history_.canRedo();
    if (continues && history_.amend())
        sequenceChanged();
    else
        recordEdit(QStringLiteral("Cambia trasformazione"), before);
    transformMergeKey_ = key;
    transformMergeCount_ = history_.undoCount();
    return true;
}

void MainWindow::fitSelectedPieces(bool fill)
{
    const core::StreamSignature* target = project_.format().video();
    const int projectW = target != nullptr ? target->width : 0;
    const int projectH = target != nullptr ? target->height : 0;
    const std::vector<int> ids = selectedPieceIds(timeline_->allSelectedObjects());
    if (ids.empty() || projectW <= 0 || projectH <= 0)
        return;
    const core::Project before = project_;
    int changed = 0;
    for (int id : ids) {
        const core::Fragment* f = project_.sequence().find(id);
        const core::Clip* clip = f != nullptr ? project_.find(f->clipId) : nullptr;
        if (clip == nullptr)
            continue;
        double factor = 1.0;
        if (fill) {
            const core::MediaSignature* signature = signatureFor(clip->path);
            const core::StreamSignature* video =
                signature != nullptr ? core::findStream(signature->streams, core::StreamKind::Video) : nullptr;
            if (video != nullptr && video->width > 0 && video->height > 0) {
                const double fx = static_cast<double>(projectW) / video->width;
                const double fy = static_cast<double>(projectH) / video->height;
                factor = std::max(fx, fy) / std::min(fx, fy);
            }
        }
        core::Transform t = f->transform;
        t.scaleX = t.scaleY = factor;
        t.posX = t.posY = 0.0;
        t.cropLeft = t.cropRight = t.cropTop = t.cropBottom = 0.0;
        changed += project_.setTransform({id}, t);
    }
    if (changed == 0)
        return;
    transformMergeKey_.clear();
    recordEdit(fill ? QStringLiteral("Riempi il fotogramma") : QStringLiteral("Adatta al fotogramma"), before);
}

}  // namespace editor::app
