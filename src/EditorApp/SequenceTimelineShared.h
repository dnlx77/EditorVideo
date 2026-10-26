#pragma once

// Misure e disegno condivisi dai file della linea del tempo del montaggio (SequenceTimeline*.cpp).

#include <QtGui/QPainter>
#include <QtCore/QRectF>

#include "EditorCore/Waveform.h"

#include <algorithm>
#include <cmath>

namespace editor::app::detail {

inline constexpr double kTop = 6;           // spazio fra il righello e la corsia video
inline constexpr double kVideoHeight = 54;    // corsia video quando c'è spazio; con poco spazio si stringe
inline constexpr double kLaneGap = 4;
inline constexpr double kHeaderWidth = 118;  // a sinistra delle corsie: nome, muto, solo, livello
inline constexpr double kTextLaneHeight = 22;   // una corsia di scritte (sopra la corsia video)
inline constexpr double kTextFadeTop = 6;       // la linea dell'opacità di una scritta: in alto = piena (sotto il bordo della corsia)
inline constexpr double kTextFadeBottom = 5;    // in basso = trasparente (sopra il bordo in basso)
inline constexpr double kTextFadeNear = 6;      // distanza orizzontale (pixel) entro cui si prende una maniglia
inline constexpr double kTextFadeNearY = 3.5;   // e verticale: stretta, così il bordo sotto la maniglia accorcia la scritta
inline constexpr double kTextFadeMinWidth = 24; // sotto questa larghezza la scritta non ha maniglie
inline constexpr double kAudioLaneHeight = 40; // altezza in più che chiede ogni traccia aggiunta

// Forma d'onda di [sourceFrom, sourceTo) di un file, disegnata in `r` (una linea verticale per pixel), solo fra
// visibleLeft e visibleRight: con lo zoom `r` può essere molto più largo della parte che si vede.
inline void drawWave(QPainter& p, const QRectF& r, const core::Waveform& wave, double sourceFrom, double sourceTo,
              const QColor& color, double visibleLeft, double visibleRight)
{
    if (r.width() < 1 || sourceTo <= sourceFrom)
        return;
    p.setPen(QPen(color, 1));
    const double middle = r.center().y();
    const double half = r.height() / 2 - 1;
    const double perPixel = (sourceTo - sourceFrom) / r.width();
    const double first = std::ceil(std::max(r.left(), visibleLeft - 1));
    const double last = std::min(r.right(), visibleRight + 1);
    for (double x = first; x < last; x += 1) {
        const double t = sourceFrom + (x - r.left()) * perPixel;
        // Radice quadrata: i suoni deboli si vedono comunque.
        const double h = std::max(0.5, std::sqrt(wave.peak(t, t + perPixel)) * half);
        p.drawLine(QPointF(x, middle - h), QPointF(x, middle + h));
    }
}

}  // namespace editor::app::detail
