#pragma once

#include <array>
#include <string>
#include <vector>

namespace editor::core {

// Trasformazione di un pezzo video: scala, posizione, rotazione, ritaglio (crop), specchio e opacità. I valori
// restano nel pezzo (Fragment::transform), quindi lo seguono se si sposta; dividendo il pezzo ognuna delle due parti
// li mantiene.
//
// Riferimento: con i valori di base (Transform{}) il fotogramma del file è adattato al fotogramma del progetto, con
// le bande nere se le proporzioni sono diverse, come senza trasformazione. Scala 1 = 100% = così com'è adattato.
struct Transform
{
    double scaleX = 1.0;  // 1 = 100%
    double scaleY = 1.0;
    // Spostamento del centro del pezzo dal centro del fotogramma del progetto, in pixel del progetto: X positiva verso
    // destra, Y positiva verso il basso.
    double posX = 0.0;
    double posY = 0.0;
    double rotation = 0.0;  // gradi, positiva in senso orario, attorno al centro del fotogramma del file
    // Ritaglio dei quattro lati come frazione del fotogramma (0 = niente, 0.25 = un quarto), come lo si vede sullo
    // schermo (con lo specchio, il lato sinistro dello schermo resta il sinistro). Il ritaglio non cambia scala né posizione
    // di quello che resta.
    double cropLeft = 0.0;
    double cropRight = 0.0;
    double cropTop = 0.0;
    double cropBottom = 0.0;
    bool flipHorizontal = false;
    bool flipVertical = false;
    double opacity = 1.0;  // 0..1

    bool operator==(const Transform&) const = default;

    // true se non cambia niente (tutti i valori di base).
    bool isIdentity() const { return *this == Transform{}; }
    // true se cambia la forma o la posizione dell'immagine (non solo l'opacità).
    bool movesPicture() const;
};

// Limiti dei valori.
inline constexpr double kMinScale = 0.01;
inline constexpr double kMaxScale = 50.0;
inline constexpr double kMaxPosition = 100000.0;
inline constexpr double kMaxRotation = 3600.0;
inline constexpr double kMaxCrop = 0.99;  // dopo il ritaglio resta almeno l'1% in larghezza e in altezza

// I valori nei loro limiti: non finiti tornano al valore di base, scala/posizione/rotazione/opacità sono limitate,
// i ritagli sono in [0, kMaxCrop] e la somma dei due lati opposti non supera kMaxCrop (si riducono in proporzione).
Transform normalized(Transform t);

// I valori cambiati rispetto a quelli di base, uno per riga e in italiano (es. "Scala 150%", "Rotazione 15°"), per il
// suggerimento del badge sul pezzo. Vuoto se non cambia niente.
std::vector<std::string> describeTransform(const Transform& transform);

struct Point2
{
    double x = 0.0;
    double y = 0.0;
};

// Trasformazione affine: da un punto del file (pixel) a uno del progetto (pixel): x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Affine
{
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

    Point2 apply(double x, double y) const { return {a * x + c * y + tx, b * x + d * y + ty}; }
    // Inversa (dal progetto al file); con una matrice non invertibile restituisce l'identità.
    Affine inverse() const;
};

// Dove va a finire un pezzo sul fotogramma del progetto.
struct TransformGeometry
{
    Affine toProject;  // dai pixel del file ai pixel del progetto (origine in alto a sinistra)
    // La parte del file che si vede dopo il ritaglio, in pixel del file.
    double sourceLeft = 0.0;
    double sourceTop = 0.0;
    double sourceRight = 0.0;
    double sourceBottom = 0.0;
    // Gli angoli di quella parte sul progetto, nell'ordine alto-sinistra, alto-destra, basso-destra, basso-sinistra
    // del FILE (con lo specchio vanno letti di conseguenza).
    std::array<Point2, 4> corners{};
    // Gli angoli del file intero (senza ritaglio), nello stesso ordine: il riquadro che serve ai manipolatori.
    std::array<Point2, 4> fullCorners{};
    double opacity = 1.0;
};

// La geometria di `t` (normalizzata) per un file di `sourceWidth` x `sourceHeight` pixel messo in un progetto di
// `projectWidth` x `projectHeight`. Misure <= 0: tutti zero (nessuna immagine).
TransformGeometry computeGeometry(const Transform& t, int sourceWidth, int sourceHeight, int projectWidth, int projectHeight);

// La parte lineare di `t` (rotazione, scala e specchio, senza spostamento: tx = ty = 0), con in più la scala `extraScale`
// (es. l'adattamento del file al progetto). Con y verso il basso un angolo positivo gira in senso orario. È la stessa
// matrice per i pezzi, le scritte, il monitor e l'esportazione.
Affine linearPart(const Transform& t, double extraScale = 1.0);

// I pixel interi che contengono i quattro punti, con un pixel di margine, limitati al fotogramma width x height:
// le colonne x0..x1-1 e le righe y0..y1-1. Vuoto se i punti cadono fuori.
struct PixelBounds
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
PixelBounds pixelBounds(const std::array<Point2, 4>& points, int width, int height);

}  // namespace editor::core
