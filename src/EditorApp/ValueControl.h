#pragma once

#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <functional>

namespace editor::app {

// Un controllo con un valore che si cambia col mouse (il fader e le manopole del Mixer e dell'equalizzatore): trascinando si
// vede il valore (onChanging) e lo si consegna al rilascio (onCommit, un solo passo di Annulla per movimento); la rotella
// fa un passo (con Maiusc un passo piccolo) e il doppio clic torna al valore di partenza, consegnandoli subito.
// Le classi figlie disegnano il controllo e dicono come il mouse e la rotella cambiano il valore.
class ValueControl : public QWidget
{
public:
    ValueControl(double minimum, double maximum, double initial, QWidget* parent = nullptr)
        : QWidget(parent), min_(minimum), max_(maximum), default_(initial), value_(std::clamp(initial, minimum, maximum))
    {
        setFocusPolicy(Qt::WheelFocus);
    }

    double value() const { return value_; }
    void setValue(double v)
    {
        v = std::clamp(v, min_, max_);
        if (v == value_)
            return;
        value_ = v;
        valueChanged();
        update();
    }
    void setDefault(double v) { default_ = v; }
    bool dragging() const { return dragging_; }

    std::function<void(double)> onChanging;  // mentre si trascina
    std::function<void(double)> onCommit;    // il valore definitivo

    // Un valore scelto dall'utente (doppio clic, rotella): lo mette e lo consegna subito.
    void turnTo(double v)
    {
        setValue(v);
        if (onChanging)
            onChanging(value_);
        if (onCommit)
            onCommit(value_);
    }

protected:
    double minimum() const { return min_; }
    double maximum() const { return max_; }

    // Il trascinamento comincia (il pulsante è appena stato premuto) e continua: le figlie chiamano changeTo col nuovo valore.
    virtual void startDrag(QMouseEvent* e) = 0;
    virtual void drag(QMouseEvent* e) = 0;
    // Il valore dopo un passo di rotella in su (`up`) o in giù; `fine` = con Maiusc.
    virtual double wheelValue(bool up, bool fine) const = 0;
    virtual void valueChanged() {}  // dopo ogni cambio del valore, prima del ridisegno

    // Durante il trascinamento: mette il valore e lo fa vedere (senza consegnarlo).
    void changeTo(double v)
    {
        setValue(v);
        if (onChanging)
            onChanging(value_);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();
        dragging_ = true;
        startDrag(e);
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (dragging_)
            drag(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !dragging_)
            return;
        dragging_ = false;
        update();
        if (onCommit)
            onCommit(value_);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        dragging_ = false;
        turnTo(default_);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const double delta = e->angleDelta().y() == 0 ? e->angleDelta().x() : e->angleDelta().y();
        if (delta == 0)
            return;
        turnTo(wheelValue(delta > 0, (e->modifiers() & Qt::ShiftModifier) != 0));
        e->accept();
    }

private:
    double min_, max_, default_, value_;
    bool dragging_ = false;
};

}  // namespace editor::app
