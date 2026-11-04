#pragma once

#include "EditorCore/Transform.h"

namespace editor::core {

// La matematica dei manipolatori sul monitor (step TR6): dove sono le maniglie di un pezzo trasformato, quale si afferra con
// il mouse e come cambia la trasformazione trascinando. Tutto in pixel del progetto (origine in alto a sinistra, come
// computeGeometry); le distanze sullo schermo si danno con `pxPerUnit` (pixel dello schermo per pixel del progetto).
struct ManipSize
{
    double srcW = 0.0;   // il fotogramma del file (conta solo la forma)
    double srcH = 0.0;
    double projW = 0.0;  // il fotogramma del progetto
    double projH = 0.0;
};

enum class ManipPart
{
    None,
    Move,    // dentro il riquadro: si sposta il pezzo
    Rotate,  // la maniglia fuori dal riquadro
    Corner,  // una maniglia d'angolo (indice 0..3: alto-sinistra, alto-destra, basso-destra, basso-sinistra del file ritagliato)
};

struct ManipHit
{
    ManipPart part = ManipPart::None;
    int corner = -1;
};

// Raggio di presa delle maniglie e distanza della maniglia di rotazione dal riquadro, in pixel dello schermo.
inline constexpr double kManipHandleRadius = 8.0;
inline constexpr double kManipRotateDistance = 24.0;

// La maniglia di rotazione: sopra il lato alto del riquadro, fuori, a kManipRotateDistance pixel dello schermo.
Point2 manipRotationHandle(const Transform& transform, const ManipSize& size, double pxPerUnit);

// Cosa c'è sotto `at`: prima le maniglie (angoli, rotazione), poi l'interno del riquadro (spostamento).
ManipHit manipHitTest(const Transform& transform, const ManipSize& size, Point2 at, double pxPerUnit);

// Spostamento: il pezzo va dove va il mouse.
Transform manipMove(const Transform& start, Point2 from, Point2 to);

// Scala trascinando l'angolo `corner`: di solito l'angolo opposto resta fermo e le proporzioni attuali si mantengono;
// `freeAspect` (Maiusc) cambia larghezza e altezza da sole; `fromCenter` (Alt) tiene fermo il centro del riquadro. Il
// risultato è nei limiti di normalized().
Transform manipScale(const Transform& start, const ManipSize& size, int corner, Point2 to, bool freeAspect, bool fromCenter);

// Rotazione attorno al centro del pezzo (il punto da cui si misura la posizione): l'angolo fra `from` e `to` si somma a quello
// di partenza; `snap` (Maiusc) arrotonda l'angolo finale a multipli di 15°.
Transform manipRotate(const Transform& start, const ManipSize& size, Point2 from, Point2 to, bool snap);

}  // namespace editor::core
