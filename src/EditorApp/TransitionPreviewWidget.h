#pragma once

#include "EditorCore/Transitions.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QTimer;

namespace editor::app {

// L'anteprima di una transizione nella finestra Sorgente: le due immagini di esempio ("A" e "B") che si passano una
// nell'altra in ciclo (A ferma, la transizione, B ferma, e da capo), col nome della transizione sotto.
class TransitionPreviewWidget : public QWidget
{
public:
    explicit TransitionPreviewWidget(QWidget* parent = nullptr);

    // Mostra questa transizione (nullptr = niente); il ciclo riparte dall'inizio.
    void setTransition(const core::Transition* transition, const core::TransitionParams& params = {});
    const core::Transition* transition() const { return transition_; }

    // Il ciclo: durata totale e avanzamento (0..1) della transizione a quel punto del ciclo (A ferma, poi la transizione,
    // poi B ferma). Fuori dal ciclo si riparte da capo.
    static double cycleSeconds();
    static double progressAt(double seconds);
    // Il fotogramma a quel punto del ciclo (immagine nulla se non c'è una transizione). Format_RGBA8888.
    QImage frameAt(double seconds) const;
    // Il punto del ciclo mostrato ora.
    double time() const { return time_; }
    void setTime(double seconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateTimer();
    void rebuildSamples();

    const core::Transition* transition_ = nullptr;
    core::TransitionParams params_;
    QSize sampleSize_;
    QImage from_;
    QImage to_;
    QImage frame_;
    double time_ = 0.0;
    qint64 startedAt_ = 0;
    QTimer* timer_ = nullptr;
};

}  // namespace editor::app
