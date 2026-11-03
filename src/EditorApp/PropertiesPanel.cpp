#include "EditorApp/PropertiesPanel.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
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
    row.spin->setMinimumWidth(90);
    auto* line = new QHBoxLayout;
    auto* caption = new QLabel(label);
    caption->setMinimumWidth(70);
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
    into->addLayout(line);
    return row;
}

void PropertiesPanel::edited(const std::string& field, std::function<void(core::Transform&)> edit)
{
    if (updating_ || !onTransformEdit)
        return;
    onTransformEdit(edit, field);
}

QToolButton* PropertiesPanel::resetButton(const QString& tip, const std::string& field, std::function<void(core::Transform&)> reset)
{
    auto* button = new QToolButton;
    button->setText(QStringLiteral("↺"));
    button->setToolTip(tip);
    button->setAutoRaise(true);
    connect(button, &QToolButton::clicked, this, [this, field, reset] { edited(field + "_ripristina", reset); });
    return button;
}

QWidget* PropertiesPanel::buildTransformPage()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(8, 8, 8, 8);

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
    quick->addWidget(fit);
    quick->addWidget(fill);
    quick->addWidget(all);
    root->addLayout(quick);
    connect(fit, &QPushButton::clicked, this, [this] {
        if (!updating_ && onFit)
            onFit(false);
    });
    connect(fill, &QPushButton::clicked, this, [this] {
        if (!updating_ && onFit)
            onFit(true);
    });
    connect(all, &QPushButton::clicked, this, [this] { edited("tutto", [](core::Transform& t) { t = core::Transform{}; }); });

    const auto group = [&](const QString& title, const std::string& field, std::function<void(core::Transform&)> reset) {
        auto* box = new QGroupBox(title);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(8, 4, 8, 8);
        auto* header = new QHBoxLayout;
        header->addStretch(1);
        header->addWidget(resetButton(QStringLiteral("Ripristina %1").arg(title.toLower()), field, std::move(reset)));
        v->addLayout(header);
        root->addWidget(box);
        return v;
    };

    // Scala
    QVBoxLayout* scale = group(QStringLiteral("Scala"), "scala", [](core::Transform& t) { t.scaleX = t.scaleY = 1.0; });
    scaleX_ = addRow(scale, QStringLiteral("Larghezza"), core::kMinScale * 100.0, core::kMaxScale * 100.0, 1.0, 1,
                     QStringLiteral(" %"), 1, 400, 1.0, "scalaX");
    linked_ = new QCheckBox(QStringLiteral("Proporzionale"));
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
    QVBoxLayout* position = group(QStringLiteral("Posizione"), "posizione", [](core::Transform& t) { t.posX = t.posY = 0.0; });
    posX_ = addRow(position, QStringLiteral("X"), -core::kMaxPosition, core::kMaxPosition, 1.0, 1, QStringLiteral(" px"), 0, 0, 1.0, "posizioneX");
    posY_ = addRow(position, QStringLiteral("Y"), -core::kMaxPosition, core::kMaxPosition, 1.0, 1, QStringLiteral(" px"), 0, 0, 1.0, "posizioneY");
    posX_.spin->setToolTip(QStringLiteral("Dal centro del fotogramma: positiva verso destra"));
    posY_.spin->setToolTip(QStringLiteral("Dal centro del fotogramma: positiva verso il basso"));
    connect(posX_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("posizioneX", [v](core::Transform& t) { t.posX = v; }); });
    connect(posY_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("posizioneY", [v](core::Transform& t) { t.posY = v; }); });

    // Rotazione
    QVBoxLayout* rotation = group(QStringLiteral("Rotazione"), "rotazione", [](core::Transform& t) { t.rotation = 0.0; });
    rotation_ = addRow(rotation, QStringLiteral("Angolo"), -core::kMaxRotation, core::kMaxRotation, 0.5, 1, QStringLiteral(" °"), -180, 180, 1.0, "rotazione");
    rotation_.spin->setToolTip(QStringLiteral("Gradi, in senso orario se positivi"));
    connect(rotation_.spin, &QDoubleSpinBox::valueChanged, this, [this](double v) { edited("rotazione", [v](core::Transform& t) { t.rotation = v; }); });

    // Ritaglio
    QVBoxLayout* crop = group(QStringLiteral("Ritaglio"), "ritaglio", [](core::Transform& t) {
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
    QVBoxLayout* look = group(QStringLiteral("Aspetto"), "aspetto", [](core::Transform& t) {
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

void PropertiesPanel::showNothing(const QString& message)
{
    message_->setText(message);
    stack_->setCurrentWidget(message_);
}

bool PropertiesPanel::showingTransform() const
{
    return stack_->currentWidget() != message_;
}

core::Transform PropertiesPanel::transform() const
{
    return shown_;
}

void PropertiesPanel::showTransform(const core::Transform& t, int count)
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
    countNote_->setText(count > 1 ? QStringLiteral("%1 pezzi scelti: ogni modifica vale per tutti (si vedono i valori del primo).").arg(count)
                                  : QString());
    countNote_->setVisible(count > 1);
    updating_ = false;
    stack_->setCurrentIndex(1);
}

}  // namespace editor::app
