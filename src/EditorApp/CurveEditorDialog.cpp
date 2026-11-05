#include "EditorApp/CurveEditorDialog.h"

#include "EditorCore/Animation.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace core = editor::core;

namespace {

constexpr double kMargin = 14.0;
constexpr double kHandleRadius = 7.0;

}  // namespace

CurveWidget::CurveWidget(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

void CurveWidget::setValues(const std::array<double, 4>& values)
{
    c_ = values;
    c_[0] = std::clamp(c_[0], 0.0, 1.0);
    c_[2] = std::clamp(c_[2], 0.0, 1.0);
    c_[1] = std::clamp(c_[1], kMinY, kMaxY);
    c_[3] = std::clamp(c_[3], kMinY, kMaxY);
    update();
}

QRectF CurveWidget::plot() const
{
    return QRectF(kMargin, kMargin, std::max(10.0, width() - 2 * kMargin), std::max(10.0, height() - 2 * kMargin));
}

QPointF CurveWidget::toWidget(double x, double y) const
{
    const QRectF r = plot();
    return QPointF(r.left() + x * r.width(), r.bottom() - (y - kMinY) / (kMaxY - kMinY) * r.height());
}

void CurveWidget::fromWidget(const QPointF& p, double& x, double& y) const
{
    const QRectF r = plot();
    x = (p.x() - r.left()) / r.width();
    y = kMinY + (r.bottom() - p.y()) / r.height() * (kMaxY - kMinY);
}

QPointF CurveWidget::handlePosition(int index) const
{
    return index == 0 ? toWidget(c_[0], c_[1]) : toWidget(c_[2], c_[3]);
}

void CurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPalette pal = palette();
    const QRectF r = plot();
    p.fillRect(rect(), pal.color(QPalette::Base));
    // Le righe a 0 (valore della chiave) e a 1 (valore della successiva), e il riquadro.
    p.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DashLine));
    for (const double y : {0.0, 1.0})
        p.drawLine(toWidget(0, y), toWidget(1, y));
    for (const double x : {0.0, 1.0})
        p.drawLine(toWidget(x, kMinY), toWidget(x, kMaxY));
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    // La diagonale della velocità costante, leggera.
    p.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DotLine));
    p.drawLine(toWidget(0, 0), toWidget(1, 1));
    // Le maniglie, collegate agli estremi.
    const QColor accent(255, 170, 0);
    p.setPen(QPen(accent, 1.5));
    p.drawLine(toWidget(0, 0), toWidget(c_[0], c_[1]));
    p.drawLine(toWidget(1, 1), toWidget(c_[2], c_[3]));
    // La curva.
    core::AnimKey key;
    key.interp = core::Interp::Bezier;
    key.c1x = c_[0];
    key.c1y = c_[1];
    key.c2x = c_[2];
    key.c2y = c_[3];
    QPainterPath path;
    constexpr int kSteps = 120;
    for (int i = 0; i <= kSteps; ++i) {
        const double u = static_cast<double>(i) / kSteps;
        const QPointF at = toWidget(u, core::easedFraction(key, u));
        if (i == 0)
            path.moveTo(at);
        else
            path.lineTo(at);
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(pal.color(QPalette::Highlight), 2.5));
    p.drawPath(path);
    // Estremi e maniglie.
    p.setPen(QPen(pal.color(QPalette::Text), 1));
    p.setBrush(pal.color(QPalette::Text));
    for (const QPointF& end : {toWidget(0, 0), toWidget(1, 1)})
        p.drawEllipse(end, 3.5, 3.5);
    for (int i = 0; i < 2; ++i) {
        p.setPen(QPen(QColor(20, 20, 20), 1.5));
        p.setBrush(dragging_ == i ? QColor(255, 220, 80) : accent);
        p.drawEllipse(handlePosition(i), kHandleRadius, kHandleRadius);
    }
}

void CurveWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    const double d0 = std::hypot(pos.x() - handlePosition(0).x(), pos.y() - handlePosition(0).y());
    const double d1 = std::hypot(pos.x() - handlePosition(1).x(), pos.y() - handlePosition(1).y());
    if (std::min(d0, d1) <= kHandleRadius + 5)
        dragging_ = d0 <= d1 ? 0 : 1;
    update();
}

void CurveWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_ < 0 || !(event->buttons() & Qt::LeftButton))
        return;
    double x = 0.0;
    double y = 0.0;
    fromWidget(event->position(), x, y);
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, kMinY, kMaxY);
    c_[static_cast<std::size_t>(dragging_ * 2)] = x;
    c_[static_cast<std::size_t>(dragging_ * 2 + 1)] = y;
    update();
    if (onChanged)
        onChanged();
}

void CurveWidget::mouseReleaseEvent(QMouseEvent*)
{
    dragging_ = -1;
    update();
}

CurveEditorDialog::CurveEditorDialog(const std::array<double, 4>& initial, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Curva del passaggio"));
    auto* root = new QVBoxLayout(this);
    auto* note = new QLabel(QStringLiteral("Trascina i due punti arancioni. In orizzontale c'è il tempo fino alla chiave dopo, in "
                                           "verticale quanto del cambio è già fatto: salendo sopra 1 o scendendo sotto 0 il valore "
                                           "supera quello della chiave (rimbalzo)."));
    note->setWordWrap(true);
    root->addWidget(note);

    presets_ = new QComboBox;
    presets_->setObjectName(QStringLiteral("curvePresets"));
    presets_->addItem(QStringLiteral("Personalizzata"));
    for (const core::BezierPreset& preset : core::bezierPresets())
        presets_->addItem(QString::fromUtf8(preset.name));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Curve pronte"), presets_);
    root->addLayout(form);

    curve_ = new CurveWidget;
    curve_->setObjectName(QStringLiteral("curveWidget"));
    curve_->setMinimumSize(300, 280);
    root->addWidget(curve_, 1);

    auto* values = new QHBoxLayout;
    const char* const names[4] = {"x1", "y1", "x2", "y2"};
    for (int i = 0; i < 4; ++i) {
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(QStringLiteral("curveValue%1").arg(i));
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setRange(i % 2 == 0 ? 0.0 : CurveWidget::kMinY, i % 2 == 0 ? 1.0 : CurveWidget::kMaxY);
        spins_[static_cast<std::size_t>(i)] = spin;
        values->addWidget(new QLabel(QString::fromLatin1(names[i])));
        values->addWidget(spin);
    }
    root->addLayout(values);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annulla"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    curve_->setValues(initial);
    curve_->onChanged = [this] { syncFromCurve(); };
    for (QDoubleSpinBox* spin : spins_)
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { syncFromSpins(); });
    connect(presets_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0)
            return;
        const core::BezierPreset& preset = core::bezierPresets()[static_cast<std::size_t>(index - 1)];
        curve_->setValues({preset.c1x, preset.c1y, preset.c2x, preset.c2y});
        syncFromCurve();
    });
    syncFromCurve();
}

void CurveEditorDialog::syncFromCurve()
{
    updating_ = true;
    const std::array<double, 4> v = curve_->values();
    for (std::size_t i = 0; i < 4; ++i)
        spins_[i]->setValue(v[i]);
    // Se coincide con una curva pronta, la si mostra; altrimenti «Personalizzata».
    int found = 0;
    const std::vector<core::BezierPreset>& presets = core::bezierPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const core::BezierPreset& p = presets[i];
        if (std::abs(p.c1x - v[0]) < 0.005 && std::abs(p.c1y - v[1]) < 0.005 && std::abs(p.c2x - v[2]) < 0.005 &&
            std::abs(p.c2y - v[3]) < 0.005)
            found = static_cast<int>(i) + 1;
    }
    presets_->setCurrentIndex(found);
    updating_ = false;
}

void CurveEditorDialog::syncFromSpins()
{
    if (updating_)
        return;
    curve_->setValues({spins_[0]->value(), spins_[1]->value(), spins_[2]->value(), spins_[3]->value()});
    updating_ = true;
    presets_->setCurrentIndex(0);
    updating_ = false;
}

}  // namespace editor::app
