#pragma once

#include "EditorCore/Transitions.h"

#include <QtCore/QSize>
#include <QtGui/QImage>

namespace editor::app {

// Le due immagini di esempio con cui si mostra una transizione (miniatura nell'elenco, anteprima nella Sorgente): "A"
// su sfondo blu, "B" su sfondo arancione. Format_RGBA8888, senza trasparenza.
QImage transitionSampleFrom(QSize size);
QImage transitionSampleTo(QSize size);

// Il fotogramma della transizione a `progress` (0..1) fra le due immagini di esempio. Format_RGBA8888.
QImage renderTransitionSample(const core::Transition& transition, const core::TransitionParams& params, double progress,
                              QSize size);

}  // namespace editor::app
