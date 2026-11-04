#pragma once

#include "EditorCore/Transform.h"

#include <vector>

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

// ---- Ritaglio sul monitor (step TR7)

// I lati e gli angoli del ritaglio si indicano con una maschera di lati: un lato = la maniglia a metà del lato, due lati
// = l'angolo. Sono i lati come si vedono sullo schermo (come cropLeft... di Transform, anche con lo specchio).
inline constexpr int kCropLeft = 1;
inline constexpr int kCropRight = 2;
inline constexpr int kCropTop = 4;
inline constexpr int kCropBottom = 8;

// Quanto resta almeno visibile del file dopo il ritaglio (frazione), perché le maniglie non si sovrappongano.
inline constexpr double kCropMinVisible = 0.05;

// Dove sta sul progetto la maniglia `edges` del riquadro ritagliato.
Point2 cropHandlePoint(const Transform& transform, const ManipSize& size, int edges);

// La maniglia sotto `at` (0 = nessuna): prima gli angoli, poi i lati (vicino al lato, non solo al suo centro).
int cropHitTest(const Transform& transform, const ManipSize& size, Point2 at, double pxPerUnit);

// Trascinando la maniglia `edges` fino a `to`, i lati coinvolti vanno dove sta il mouse (misurato sull'immagine, quindi
// anche se il pezzo è ruotato, scalato o specchiato), restando dentro il file e lasciando kCropMinVisible visibile.
Transform manipCrop(const Transform& start, const ManipSize& size, int edges, Point2 to);

// ---- Guide magnetiche (step TR7)

// Le linee dello schermo su cui è scattato lo spostamento, in pixel del progetto: x di guide verticali, y di orizzontali.
struct SnapGuides
{
    std::vector<double> vertical;
    std::vector<double> horizontal;
};

// Spostamento: il bordo sinistro/destro/alto/basso e il centro del pezzo scattano sui bordi e sul centro del fotogramma
// quando ne distano meno di kSnapDistance pixel dello schermo.
inline constexpr double kSnapDistance = 6.0;
Transform snapMove(const Transform& moved, const ManipSize& size, double pxPerUnit, SnapGuides* guides);

// Rotazione: scatta a 0, 90, 180, 270 gradi (e ai loro multipli) entro kSnapDegrees. `snapped` dice se è scattata.
inline constexpr double kSnapDegrees = 2.0;
Transform snapRotation(const Transform& rotated, bool* snapped);

// Scala: scatta al 100% entro kSnapScale (frazione), separatamente per larghezza e altezza.
inline constexpr double kSnapScale = 0.02;
Transform snapScale(const Transform& scaled);

}  // namespace editor::core
