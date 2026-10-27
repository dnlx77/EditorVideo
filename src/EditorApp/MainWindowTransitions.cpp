#include "EditorApp/MainWindow.h"

#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/UiText.h"
#include "EditorCore/Transitions.h"

#include <QtWidgets/QCheckBox>
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
    const core::TransitionClip* clip = project_.transitions().find(id);
    if (clip == nullptr)
        return;
    const double limit = std::max(core::kMinTransition, project_.transitionLimits(clip->beforeFragment).max());

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Transizione"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* type = new QComboBox;
    for (const core::Transition* transition : core::transitionRegistry())
        type->addItem(QString::fromUtf8(transition->name()), QString::fromUtf8(transition->id()));
    type->setCurrentIndex(std::max(0, type->findData(QString::fromUtf8(clip->type.c_str()))));
    form->addRow(QStringLiteral("Tipo"), type);
    auto* direction = new QComboBox;
    direction->addItem(QStringLiteral("Verso sinistra"), static_cast<int>(core::TransitionDirection::Left));
    direction->addItem(QStringLiteral("Verso destra"), static_cast<int>(core::TransitionDirection::Right));
    direction->addItem(QStringLiteral("Verso l'alto"), static_cast<int>(core::TransitionDirection::Up));
    direction->addItem(QStringLiteral("Verso il basso"), static_cast<int>(core::TransitionDirection::Down));
    direction->setCurrentIndex(std::max(0, direction->findData(static_cast<int>(clip->params.direction))));
    form->addRow(QStringLiteral("Direzione"), direction);
    const auto syncDirection = [type, direction] {
        const core::Transition* chosen = core::findTransition(type->currentData().toString().toStdString());
        direction->setEnabled(chosen != nullptr && chosen->usesDirection());
    };
    connect(type, &QComboBox::currentIndexChanged, &dialog, syncDirection);
    syncDirection();
    auto* seconds = new QDoubleSpinBox;
    seconds->setDecimals(2);
    seconds->setSingleStep(0.1);
    seconds->setRange(core::kMinTransition, limit);
    seconds->setSuffix(QStringLiteral(" s"));
    seconds->setValue(std::min(clip->duration, limit));
    seconds->setToolTip(QStringLiteral("Al massimo quanto permettono i due pezzi e il video che c'è oltre i loro bordi"));
    form->addRow(QStringLiteral("Durata"), seconds);
    auto* audio = new QCheckBox(QStringLiteral("Dissolvenza incrociata anche dell'audio"));
    audio->setChecked(clip->crossfadeAudio);
    form->addRow(QString(), audio);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const core::Project before = project_;
    core::TransitionParams params;
    params.direction = static_cast<core::TransitionDirection>(direction->currentData().toInt());
    const bool changedSettings =
        project_.setTransitionSettings(id, type->currentData().toString().toStdString(), params, audio->isChecked());
    const bool changedDuration = project_.setTransitionDuration(id, seconds->value());
    if (changedSettings || changedDuration)
        recordEdit(QStringLiteral("Modifica transizione"), before);
}

}  // namespace editor::app
