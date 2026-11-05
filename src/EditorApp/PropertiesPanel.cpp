#include "EditorApp/PropertiesPanel.h"
#include "EditorApp/AnimColors.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace editor::app {

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent)
{
    stack_ = new QStackedWidget;
    message_ = new QLabel;
    message_->setWordWrap(true);
    message_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    message_->setMargin(12);
    stack_->addWidget(message_);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(buildTransformPage());
    stack_->addWidget(scroll);
    transformPage_ = scroll;
    transitionPage_ = buildTransitionPage();
    stack_->addWidget(transitionPage_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);
    setMinimumWidth(230);
    showNothing(QStringLiteral("Scegli un oggetto sulla linea del tempo per vederne e cambiarne le proprietà."));
}

PropertiesPanel::Row PropertiesPanel::addRow(QVBoxLayout* into, const QString& label, double min, double max, double step,
                                             int decimals, const QString& suffix, int sliderMin, int sliderMax,
                                             double sliderScale, const char* name)
{
    Row row;
    row.spin = new QDoubleSpinBox;
    row.spin->setRange(min, max);
    row.spin->setSingleStep(step);
    row.spin->setDecimals(decimals);
    row.spin->setSuffix(suffix);
    row.spin->setObjectName(QString::fromLatin1(name));
    row.spin->setKeyboardTracking(false);  // si applica con Invio o uscendo dal campo, non a ogni cifra
    // Stile minimale: caselle strette e basse, senza frecce.
    row.spin->setFixedWidth(70);
    row.spin->setFixedHeight(20);
    row.spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    row.spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* line = new QHBoxLayout;
    line->setSpacing(4);
    auto* caption = new QLabel(label);
    caption->setFixedWidth(60);
    line->addWidget(caption);
    line->addWidget(row.spin);
    if (sliderMax > sliderMin) {
        row.slider = new QSlider(Qt::Horizontal);
        row.slider->setRange(sliderMin, sliderMax);
        row.slider->setObjectName(QString::fromLatin1(name) + QStringLiteral("_cursore"));
        line->addWidget(row.slider, 1);
        QDoubleSpinBox* spin = row.spin;
        QSlider* slider = row.slider;
        connect(slider, &QSlider::valueChanged, this, [spin, sliderScale](int v) { spin->setValue(v / sliderScale); });
        connect(spin, &QDoubleSpinBox::valueChanged, this, [slider, sliderScale](double v) {
            const QSignalBlocker block(slider);
            slider->setValue(static_cast<int>(std::lround(v * sliderScale)));
        });
    }
    else
        line->addStretch(1);
    into->addLayout(line);
    return row;
}

void PropertiesPanel::edited(const std::string& field, std::function<void(core::Transform&)> edit)
{
    if (updating_ || !onTransformEdit)
        return;
    onTransformEdit(edit, field);
}

void PropertiesPanel::animate(core::AnimGroup group, AnimAction action)
{
    if (!updating_ && onAnimationAction)
        onAnimationAction(group, action);
}

QToolButton* PropertiesPanel::resetButton(const QString& tip, const std::string& field, core::AnimGroup group,
                                          std::function<void(core::Transform&)> reset)
{
    auto* button = new QToolButton;
    button->setText(QStringLiteral("↺"));
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setFixedSize(18, 18);
    connect(button, &QToolButton::clicked, this, [this, field, group, reset] {
        // Con un'animazione il ripristino la spegne (valori di base): non è una modifica che mette una chiave.
        if (groupAnimated_[static_cast<std::size_t>(group)])
            animate(group, AnimAction::Reset);
        else
            edited(field + "_ripristina", reset);
    });
    return button;
}

QWidget* PropertiesPanel::buildTransformPage()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(2);
    // Testo un po' più piccolo in tutta la pagina.
    {
        QFont small = page->font();
        small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
        page->setFont(small);
    }

    countNote_ = new QLabel;
    countNote_->setWordWrap(true);
    root->addWidget(countNote_);

    // Adatta / Riempi / Ripristina tutto.
    auto* quick = new QHBoxLayout;
    auto* fit = new QPushButton(QStringLiteral("Adatta"));
    fit->setToolTip(QStringLiteral("Scala 100%: tutta l'immagine nel fotogramma (bande nere se le proporzioni sono diverse)"));
    auto* fill = new QPushButton(QStringLiteral("Riempi"));
    fill->setToolTip(QStringLiteral("Ingrandisce l'immagine fino a riempire il fotogramma (i bordi in eccesso restano fuori)"));
    auto* all = new QPushButton(QStringLiteral("Ripristina tutto"));
    for (QPushButton* button : {fit, fill, all})
        button->setFixedHeight(22);
    quick->addWidget(fit);
    quick->addWidget(fill);
    quick->addWidget(all);
    root->addLayout(quick);
    // Preset di animazione: zoom lento, panoramica, dissolvenze (menu; l'animazione si può poi ritoccare con i rombi).
    presetButton_ = new QToolButton;
    presetButton_->setObjectName(QStringLiteral("presetAnimazione"));
    presetButton_->setText(QStringLiteral("Animazione ▾"));
    presetButton_->setPopupMode(QToolButton::InstantPopup);
    presetButton_->setToolTip(QStringLiteral("Animazioni pronte per il pezzo scelto: zoom lento, panoramica, dissolvenze. Poi si ritoccano con i rombi."));
    presetButton_->setFixedHeight(22);
    presetButton_->setEnabled(false);
    auto* presetMenu = new QMenu(presetButton_);
    for (int i = 0; i < core::kAnimPresetCount; ++i) {
        const auto preset = static_cast<core::AnimPreset>(i);
        QAction* action = presetMenu->addAction(QString::fromUtf8(core::animPresetName(preset)));
        connect(action, &QAction::triggered, this, [this, preset] {
            if (!updating_ && onAnimationPreset)
                onAnimationPreset(preset);
        });
        if (preset == core::AnimPreset::ZoomOut || preset == core::AnimPreset::PanRight)
            presetMenu->addSeparator();
    }
    presetButton_->setMenu(presetMenu);
    root->addWidget(presetButton_);
    mini_ = new AnimMiniTimeline;
    root->addWidget(mini_);
    connect(fit, &QPushButton::clicked, this, [this] {
        if (!updating_ && onFit)
            onFit(false);
    });
    connect(fill, &QPushButton::clicked, this, [this] {
        if (!updating_ && onFit)
            onFit(true);
    });
    connect(all, &QPushButton::clicked, this, [this] {
        if (std::any_of(groupAnimated_.begin(), groupAnimated_.end(), [](bool animated) { return animated; }))
            animate(core::AnimGroup::Scale, AnimAction::ResetAll);
        else
            edited("tutto", [](core::Transform& t) { t = core::Transform{}; });
    });

    const auto group = [&](const QString& title, const std::string& field, core::AnimGroup animGroup,
                           std::function<void(core::Transform&)> reset) {
        // Una sezione senza riquadro: una riga sottile sopra, poi il titolo (con il ripristino a destra) e i campi.
        auto* separator = new QFrame;
        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setEnabled(false);
        root->addSpacing(2);
        root->addWidget(separator);
        auto* section = new QWidget;
        auto* v = new QVBoxLayout(section);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        auto* header = new QHBoxLayout;
        auto* heading = new QLabel(title);
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        header->addWidget(heading);
        header->addStretch(1);
        // Fotogrammi chiave: freccia indietro, rombo, freccia avanti.
        AnimButtons& anim = animButtons_[static_cast<std::size_t>(animGroup)];
        const auto small = [&](const QString& text, const QString& tip, const char* name) {
            auto* button = new QToolButton;
            button->setText(text);
            button->setToolTip(tip);
            button->setAutoRaise(true);
            button->setFixedSize(18, 18);
            button->setObjectName(QString::fromLatin1(name) + QString::number(static_cast<int>(animGroup)));
            return button;
        };
        anim.prev = small(QStringLiteral("◀"), QStringLiteral("Vai al fotogramma chiave prima"), "chiavePrima");
        anim.key = small(QStringLiteral("◇"), QStringLiteral("Anima %1: mette un fotogramma chiave sul cursore").arg(title.toLower()), "chiave");
        anim.next = small(QStringLiteral("▶"), QStringLiteral("Vai al fotogramma chiave dopo"), "chiaveDopo");
        connect(anim.prev, &QToolButton::clicked, this, [this, animGroup] { animate(animGroup, AnimAction::PrevKey); });
        connect(anim.key, &QToolButton::clicked, this, [this, animGroup] { animate(animGroup, AnimAction::ToggleKey); });
        connect(anim.next, &QToolButton::clicked, this, [this, animGroup] { animate(animGroup, AnimAction::NextKey); });
        header->addWidget(anim.prev);
        header->addWidget(anim.key);
        header->addWidget(anim.next);
        anim.prev->setEnabled(false);
        anim.key->setEnabled(false);
        anim.next->setEnabled(false);
        header->addWidget(resetButton(QStringLiteral("Ripristina %1").arg(title.toLower()), field, animGroup, std::move(reset)));
        v->addLayout(header);
        root->addWidget(section);
        return v;
    };

    // Scala
    QVBoxLayout* scale = group(QStringLiteral("Scala"), "scala", core::AnimGroup::Scale, [](core::Transform& t) { t.scaleX = t.scaleY = 1.0; });
    scaleX_ = addRow(scale, QStringLiteral("Larghezza"), core::kMinScale * 100.0, core::kMaxScale * 100.0, 1.0, 1,
                     QStringLiteral(" %"), 1, 400, 1.0, "scalaX");
    linked_ = new QCheckBox(QStringLiteral("Proporzionale"));
    linked_->setObjectName(QStringLiteral("proporzionale"));
    linked_->setChecked(true);
    linked_->setToolTip(QStringLiteral("Larghezza e altezza cambiano insieme"));
    scale->addWidget(linked_);
    scaleY_ = addRow(scale, QStringLiteral("Altezza"), core::kMinScale * 100.0, core::kMaxScale * 100.0, 1.0, 1,
                     QStringLiteral(" %"), 1, 400, 1.0, "scalaY");
    connect(scaleX_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const bool both = linked_->isChecked();
        edited("scala", [v, both](core::Transform& t) {
            t.scaleX = v / 100.0;
            if (both)
                t.scaleY = v / 100.0;
        });
    });
    connect(scaleY_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const bool both = linked_->isChecked();
        edited("scala", [v, both](core::Transform& t) {
            t.scaleY = v / 100.0;
            if (both)
                t.scaleX = v / 100.0;
        });
    });

    // Posizione
    QVBoxLayout* position = group(QStringLiteral("Posizione"), "posizione", core::AnimGroup::Position, [](core::Transform& t) { t.posX = t.posY = 0.0; });
    posX_ = addRow(position, QStringLiteral("X"), -core::kMaxPosition, core::kMaxPosition, 1.0, 1, QStringLiteral(" px"), 0, 0, 1.0, "posizioneX");
    posY_ = addRow(position, QStringLiteral("Y"), -core::kMaxPosition, core::kMaxPosition, 1.0, 1, QStringLiteral(" px"), 0, 0, 1.0, "posizioneY");
    posX_.spin->setToolTip(QStringLiteral("Dal centro del fotogramma: positiva verso destra"));
    posY_.spin->setToolTip(QStringLiteral("Dal centro del fotogramma: positiva verso il basso"));
    connect(posX_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("posizioneX", [v](core::Transform& t) { t.posX = v; }); });
    connect(posY_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("posizioneY", [v](core::Transform& t) { t.posY = v; }); });

    // Rotazione
    QVBoxLayout* rotation = group(QStringLiteral("Rotazione"), "rotazione", core::AnimGroup::Rotation, [](core::Transform& t) { t.rotation = 0.0; });
    rotation_ = addRow(rotation, QStringLiteral("Angolo"), -core::kMaxRotation, core::kMaxRotation, 0.5, 1, QStringLiteral(" °"), -180, 180, 1.0, "rotazione");
    rotation_.spin->setToolTip(QStringLiteral("Gradi, in senso orario se positivi"));
    connect(rotation_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("rotazione", [v](core::Transform& t) { t.rotation = v; }); });

    // Ritaglio
    QVBoxLayout* crop = group(QStringLiteral("Ritaglio"), "ritaglio", core::AnimGroup::Crop, [](core::Transform& t) {
        t.cropLeft = t.cropRight = t.cropTop = t.cropBottom = 0.0;
    });
    const auto cropRow = [&](Row& row, const QString& label, const char* name, double core::Transform::*member) {
        row = addRow(crop, label, 0.0, core::kMaxCrop * 100.0, 0.5, 1, QStringLiteral(" %"), 0, 99, 1.0, name);
        connect(row.spin, &QDoubleSpinBox::valueChanged, this, [this, name, member](double v) {
            edited(name, [v, member](core::Transform& t) { t.*member = v / 100.0; });
        });
    };
    cropRow(cropLeft_, QStringLiteral("Sinistra"), "ritaglioSinistra", &core::Transform::cropLeft);
    cropRow(cropRight_, QStringLiteral("Destra"), "ritaglioDestra", &core::Transform::cropRight);
    cropRow(cropTop_, QStringLiteral("Alto"), "ritaglioAlto", &core::Transform::cropTop);
    cropRow(cropBottom_, QStringLiteral("Basso"), "ritaglioBasso", &core::Transform::cropBottom);

    // Aspetto
    QVBoxLayout* look = group(QStringLiteral("Aspetto"), "aspetto", core::AnimGroup::Opacity, [](core::Transform& t) {
        t.opacity = 1.0;
        t.flipHorizontal = t.flipVertical = false;
    });
    opacity_ = addRow(look, QStringLiteral("Opacità"), 0.0, 100.0, 1.0, 0, QStringLiteral(" %"), 0, 100, 1.0, "opacita");
    connect(opacity_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("opacita", [v](core::Transform& t) { t.opacity = v / 100.0; }); });
    auto* flips = new QHBoxLayout;
    flipH_ = new QToolButton;
    flipH_->setText(QStringLiteral("Specchia ↔"));
    flipH_->setCheckable(true);
    flipH_->setToolTip(QStringLiteral("Specchia in orizzontale"));
    flipV_ = new QToolButton;
    flipV_->setText(QStringLiteral("Specchia ↕"));
    flipV_->setCheckable(true);
    flipV_->setToolTip(QStringLiteral("Specchia in verticale"));
    flips->addWidget(flipH_);
    flips->addWidget(flipV_);
    flips->addStretch(1);
    look->addLayout(flips);
    connect(flipH_, &QToolButton::toggled, this, [this](bool on) { edited("specchioOrizzontale", [on](core::Transform& t) { t.flipHorizontal = on; }); });
    connect(flipV_, &QToolButton::toggled, this, [this](bool on) { edited("specchioVerticale", [on](core::Transform& t) { t.flipVertical = on; }); });

    root->addStretch(1);
    return page;
}

QWidget* PropertiesPanel::buildTransitionPage()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("<b>Transizione</b>"));
    root->addWidget(title);
    auto* form = new QFormLayout;
    root->addLayout(form);
    transitionType_ = new QComboBox;
    transitionType_->setObjectName(QStringLiteral("transizioneTipo"));
    for (const core::Transition* transition : core::transitionRegistry())
        transitionType_->addItem(QString::fromUtf8(transition->name()), QString::fromUtf8(transition->id()));
    form->addRow(QStringLiteral("Tipo"), transitionType_);
    transitionDirection_ = new QComboBox;
    transitionDirection_->setObjectName(QStringLiteral("transizioneDirezione"));
    transitionDirection_->addItem(QStringLiteral("Verso sinistra"), static_cast<int>(core::TransitionDirection::Left));
    transitionDirection_->addItem(QStringLiteral("Verso destra"), static_cast<int>(core::TransitionDirection::Right));
    transitionDirection_->addItem(QStringLiteral("Verso l'alto"), static_cast<int>(core::TransitionDirection::Up));
    transitionDirection_->addItem(QStringLiteral("Verso il basso"), static_cast<int>(core::TransitionDirection::Down));
    form->addRow(QStringLiteral("Direzione"), transitionDirection_);
    transitionDuration_ = new QDoubleSpinBox;
    transitionDuration_->setObjectName(QStringLiteral("transizioneDurata"));
    transitionDuration_->setDecimals(2);
    transitionDuration_->setFixedWidth(70);
    transitionDuration_->setFixedHeight(20);
    transitionDuration_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    transitionDuration_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    transitionDuration_->setSingleStep(0.1);
    transitionDuration_->setSuffix(QStringLiteral(" s"));
    transitionDuration_->setToolTip(QStringLiteral("Al massimo quanto permettono i due pezzi e il video che c'è oltre i loro bordi"));
    form->addRow(QStringLiteral("Durata"), transitionDuration_);
    transitionAudio_ = new QCheckBox(QStringLiteral("Dissolvenza incrociata anche dell'audio"));
    transitionAudio_->setObjectName(QStringLiteral("transizioneAudio"));
    root->addWidget(transitionAudio_);
    transitionHint_ = new QLabel;
    transitionHint_->setWordWrap(true);
    root->addWidget(transitionHint_);
    root->addStretch(1);

    connect(transitionType_, &QComboBox::currentIndexChanged, this, [this](int) {
        const core::Transition* chosen = core::findTransition(transitionType_->currentData().toString().toStdString());
        transitionDirection_->setEnabled(chosen != nullptr && chosen->usesDirection());
        transitionEdited("tipo");
    });
    connect(transitionDirection_, &QComboBox::currentIndexChanged, this, [this](int) { transitionEdited("direzione"); });
    connect(transitionDuration_, &QDoubleSpinBox::valueChanged, this, [this](double) { transitionEdited("durata"); });
    connect(transitionAudio_, &QCheckBox::toggled, this, [this](bool) { transitionEdited("audio"); });
    return page;
}

void PropertiesPanel::transitionEdited(const std::string& field)
{
    if (updating_ || !onTransitionEdit)
        return;
    core::TransitionParams params;
    params.direction = static_cast<core::TransitionDirection>(transitionDirection_->currentData().toInt());
    onTransitionEdit(transitionType_->currentData().toString().toStdString(), params, transitionDuration_->value(),
                     transitionAudio_->isChecked(), field);
}

void PropertiesPanel::showTransitionClip(const core::TransitionClip& clip, double minDuration, double maxDuration)
{
    updating_ = true;
    {
        const QSignalBlocker a(transitionType_);
        const QSignalBlocker b(transitionDirection_);
        const QSignalBlocker c(transitionDuration_);
        const QSignalBlocker d(transitionAudio_);
        const int type = transitionType_->findData(QString::fromUtf8(clip.type.c_str()));
        if (type >= 0 && transitionType_->currentIndex() != type)
            transitionType_->setCurrentIndex(type);
        const int direction = transitionDirection_->findData(static_cast<int>(clip.params.direction));
        if (direction >= 0 && transitionDirection_->currentIndex() != direction)
            transitionDirection_->setCurrentIndex(direction);
        const core::Transition* chosen = core::findTransition(clip.type);
        transitionDirection_->setEnabled(chosen != nullptr && chosen->usesDirection());
        maxDuration = std::max(minDuration, maxDuration);
        if (std::abs(transitionDuration_->minimum() - minDuration) > 1e-9 || std::abs(transitionDuration_->maximum() - maxDuration) > 1e-9)
            transitionDuration_->setRange(minDuration, maxDuration);
        const double duration = std::min(clip.duration, maxDuration);
        if (std::abs(transitionDuration_->value() - duration) > 1e-9)
            transitionDuration_->setValue(duration);
        if (transitionAudio_->isChecked() != clip.crossfadeAudio)
            transitionAudio_->setChecked(clip.crossfadeAudio);
    }
    updating_ = false;
    stack_->setCurrentWidget(transitionPage_);
}

bool PropertiesPanel::showingTransitionClip() const
{
    return stack_->currentWidget() == transitionPage_;
}

void PropertiesPanel::showNothing(const QString& message)
{
    message_->setText(message);
    stack_->setCurrentWidget(message_);
}

bool PropertiesPanel::showingTransform() const
{
    return stack_->currentWidget() == transformPage_;
}

void PropertiesPanel::setTextPage(QWidget* page)
{
    textPage_ = page;
    stack_->addWidget(page);
}

void PropertiesPanel::showText()
{
    if (textPage_ != nullptr)
        stack_->setCurrentWidget(textPage_);
}

bool PropertiesPanel::showingText() const
{
    return textPage_ != nullptr && stack_->currentWidget() == textPage_;
}

core::Transform PropertiesPanel::transform() const
{
    return shown_;
}

void PropertiesPanel::showTransform(const core::Transform& t, int count)
{
    showTransform(t, count, AnimationState{});
    mini_->setData(0.0, 1.0, -1.0, {});
}

void PropertiesPanel::showTransform(const core::Transform& t, int count, const AnimationState& animation)
{
    shown_ = t;
    updating_ = true;
    const auto set = [](Row& row, double value) {
        if (std::abs(row.spin->value() - value) > 1e-9) {
            const QSignalBlocker blockSpin(row.spin);
            row.spin->setValue(value);
        }
        if (row.slider != nullptr) {
            const QSignalBlocker blockSlider(row.slider);
            row.slider->setValue(static_cast<int>(std::lround(value)));
        }
    };
    set(scaleX_, t.scaleX * 100.0);
    set(scaleY_, t.scaleY * 100.0);
    set(posX_, t.posX);
    set(posY_, t.posY);
    set(rotation_, t.rotation);
    set(cropLeft_, t.cropLeft * 100.0);
    set(cropRight_, t.cropRight * 100.0);
    set(cropTop_, t.cropTop * 100.0);
    set(cropBottom_, t.cropBottom * 100.0);
    set(opacity_, t.opacity * 100.0);
    {
        const QSignalBlocker a(flipH_);
        const QSignalBlocker b(flipV_);
        flipH_->setChecked(t.flipHorizontal);
        flipV_->setChecked(t.flipVertical);
    }
    // Scale diverse: la catenella si apre da sola, così non si cambia per sbaglio quella dell'altro lato.
    if (std::abs(t.scaleX - t.scaleY) > 1e-9) {
        const QSignalBlocker c(linked_);
        linked_->setChecked(false);
    }
    if (presetButton_ != nullptr)
        presetButton_->setEnabled(animation.presets);
    for (std::size_t g = 0; g < animButtons_.size(); ++g) {
        const core::GroupState& state = animation.groups[g];
        groupAnimated_[g] = state.animated;
        AnimButtons& b = animButtons_[g];
        b.key->setEnabled(animation.enabled);
        b.prev->setEnabled(animation.enabled && state.hasPrev);
        b.next->setEnabled(animation.enabled && state.hasNext);
        b.key->setText(state.keyAtCursor ? QStringLiteral("◆") : QStringLiteral("◇"));
        b.key->setStyleSheet(state.animated ? QStringLiteral("QToolButton { color: %1; }")
                                                    .arg(animGroupColor(static_cast<core::AnimGroup>(g)).name())
                                            : QString());
        b.key->setToolTip(state.keyAtCursor ? QStringLiteral("Toglie il fotogramma chiave sul cursore")
                          : state.animated  ? QStringLiteral("Mette un fotogramma chiave sul cursore (col valore di adesso)")
                                            : QStringLiteral("Anima: mette un fotogramma chiave sul cursore"));
    }
    countNote_->setText(count > 1 ? QStringLiteral("%1 pezzi scelti: ogni modifica vale per tutti (si vedono i valori del primo).").arg(count)
                                  : QString());
    countNote_->setVisible(count > 1);
    updating_ = false;
    stack_->setCurrentWidget(transformPage_);
}

}  // namespace editor::app
