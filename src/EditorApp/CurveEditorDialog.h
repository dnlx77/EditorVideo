#pragma once

#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>

#include <array>
#include <functional>

class QComboBox;
class QDoubleSpinBox;

namespace editor::app {

// Il disegno della curva di un passaggio: il tempo del tratto in orizzontale (0 = chiave, 1 = chiave dopo), quanto del cambio
// di valore è fatto in verticale (0 = valore della chiave, 1 = valore della successiva; si può uscire per fare un rimbalzo).
// Due maniglie (i punti di controllo della Bézier) si trascinano con il mouse.
class CurveWidget : public QWidget
{
public:
    explicit CurveWidget(QWidget* parent = nullptr);

    // c1x, c1y, c2x, c2y.
    std::array<double, 4> values() const { return c_; }
    void setValues(const std::array<double, 4>& values);  // non chiama onChanged
    std::function<void()> onChanged;                      // una maniglia è stata trascinata

    QSize sizeHint() const override { return {320, 300}; }
    QSize minimumSizeHint() const override { return {200, 200}; }
    // Solo per le prove: dove sta una maniglia (0 = la prima, 1 = la seconda), in coordinate del widget.
    QPointF handlePosition(int index) const;

    static constexpr double kMinY = -0.8;  // quanto si può uscire sotto 0 e sopra 1
    static constexpr double kMaxY = 1.8;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF plot() const;  // il riquadro 0..1 in orizzontale, kMinY..kMaxY in verticale
    QPointF toWidget(double x, double y) const;
    void fromWidget(const QPointF& p, double& x, double& y) const;

    std::array<double, 4> c_{0.25, 0.1, 0.25, 1.0};
    int dragging_ = -1;
};

// La finestra «Curva del passaggio»: la curva da trascinare, un elenco di curve pronte e i quattro valori scritti. Con OK i
// valori scelti si prendono con values().
class CurveEditorDialog : public QDialog
{
public:
    explicit CurveEditorDialog(const std::array<double, 4>& initial, QWidget* parent = nullptr);

    std::array<double, 4> values() const { return curve_->values(); }
    CurveWidget* curveWidget() const { return curve_; }

private:
    void syncFromCurve();
    void syncFromSpins();

    CurveWidget* curve_ = nullptr;
    QComboBox* presets_ = nullptr;
    std::array<QDoubleSpinBox*, 4> spins_{};
    bool updating_ = false;
};

}  // namespace editor::app
