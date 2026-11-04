#include "EditorApp/MainWindow.h"

#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/UiText.h"
#include "EditorCore/Transitions.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace editor::app {

// Le transizioni (step X5): sulla linea del tempo, trascinate dall'elenco; durata, tipo e direzione con Annulla/Ripeti.

namespace {

constexpr double kDefaultTransition = 1.0;  // una transizione nuova dura 1 secondo (se i margini bastano)

}  // namespace

void MainWindow::refreshTransitions()
{
    std::vector<SequenceTimeline::TransitionBlock> blocks;
    for (const core::TransitionPlacement& placed : project_.activeTransitions()) {
        const core::TransitionClip* clip = project_.transitions().find(placed.id);
        const core::Transition* transition = clip != nullptr ? core::findTransition(clip->type) : nullptr;
        if (transition == nullptr)
            continue;
        blocks.push_back({placed.id, QString::fromUtf8(transition->name()), placed.start(), placed.end(), placed.junction});
    }
    timeline_->setTransitions(std::move(blocks));
}

bool MainWindow::canDropTransition(int beforeFragment) const
{
    return project_.transitions().atJunction(beforeFragment) == nullptr
           && project_.transitionLimits(beforeFragment).max() >= core::kMinTransition - core::kTimeEpsilon;
}

void MainWindow::refuseTransition(int beforeFragment)
{
    QString why;
    if (beforeFragment == 0)
        why = QStringLiteral("Trascina la transizione sulla giunzione fra due pezzi attaccati.");
    else if (project_.transitions().atJunction(beforeFragment) != nullptr)
        why = QStringLiteral("Su questa giunzione c'è già una transizione: fai doppio clic su di lei per cambiarla.");
    else
        why = QStringLiteral("Qui la transizione non ci sta: servono almeno %1 s di video oltre i bordi dei due pezzi (e i "
                             "pezzi attaccati, senza spazio vuoto).")
                  .arg(core::kMinTransition);
    statusBar()->showMessage(why, 8000);
}

int MainWindow::addTransition(const QString& type, int beforeFragment)
{
    const core::Project before = project_;
    const double duration = std::min(kDefaultTransition, project_.transitionLimits(beforeFragment).max());
    const int id = project_.addTransition(beforeFragment, type.toStdString(), duration);
    if (id == 0) {
        refuseTransition(beforeFragment);
        return 0;
    }
    recordEdit(QStringLiteral("Aggiungi transizione"), before);
    timeline_->setSelectedTransition(id);
    refreshPropertiesPanel();
    statusBar()->showMessage(QStringLiteral("Transizione messa sulla giunzione (%1 s): trascina i suoi bordi o fai doppio clic "
                                            "per cambiarla, Canc per toglierla.")
                                 .arg(QString::number(duration, 'f', 2)),
                             8000);
    return id;
}

double MainWindow::clampTransitionDuration(int id, double desired) const
{
    const core::TransitionClip* clip = project_.transitions().find(id);
    if (clip == nullptr)
        return desired;
    return std::clamp(desired, core::kMinTransition, std::max(core::kMinTransition, project_.transitionLimits(clip->beforeFragment).max()));
}

bool MainWindow::changeTransitionDuration(int id, double duration)
{
    const core::TransitionClip* clip = project_.transitions().find(id);
    if (clip == nullptr || std::abs(clip->duration - duration) < 1e-6)
        return false;
    const core::Project before = project_;
    if (!project_.setTransitionDuration(id, clampTransitionDuration(id, duration)))
        return false;
    recordEdit(QStringLiteral("Cambia durata della transizione"), before);
    return true;
}

bool MainWindow::removeTransition(int id)
{
    const core::Project before = project_;
    if (!project_.removeTransition(id))
        return false;
    timeline_->setSelectedTransition(0);
    recordEdit(QStringLiteral("Togli transizione"), before);
    return true;
}

void MainWindow::editTransition(int id)
{
    // Doppio clic sul blocco: le proprietà della transizione sono nel pannello Proprietà, che si mostra.
    if (project_.transitions().find(id) == nullptr)
        return;
    timeline_->setSelectedTransition(id);
    if (fullScreenDock_ != nullptr)
        exitFullScreen();
    propertiesDock_->show();
    propertiesDock_->raise();
    refreshPropertiesPanel();
}

bool MainWindow::applyTransitionEdit(const std::string& type, const core::TransitionParams& params, double duration,
                                     bool crossfadeAudio, const std::string& field)
{
    const int id = timeline_->selectedTransition();
    const core::TransitionClip* clip = project_.transitions().find(id);
    if (clip == nullptr)
        return false;
    const core::Project before = project_;
    const bool changedSettings = project_.setTransitionSettings(id, type, params, crossfadeAudio);
    const bool changedDuration = std::abs(clip->duration - duration) > 1e-6
                              && project_.setTransitionDuration(id, clampTransitionDuration(id, duration));
    if (!changedSettings && !changedDuration)
        return false;
    // Girare più volte di fila lo stesso controllo è una sola voce di Annulla.
    const std::string key = field + ':' + std::to_string(id);
    const bool continues = transitionMergeKey_ == key && transitionMergeCount_ == history_.undoCount() && history_.canUndo()
                        && history_.undoLabel() == "Modifica transizione" && !history_.canRedo();
    if (continues && history_.amend())
        sequenceChanged();
    else
        recordEdit(QStringLiteral("Modifica transizione"), before);
    transitionMergeKey_ = key;
    transitionMergeCount_ = history_.undoCount();
    return true;
}

}  // namespace editor::app
