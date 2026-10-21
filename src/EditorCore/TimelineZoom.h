#pragma once

namespace editor::core {

// Zoom orizzontale di una linea del tempo: quanti pixel per secondo e da quale secondo parte la parte visibile.
// Le x sono in pixel dal bordo sinistro della traccia (la parte della linea del tempo che rappresenta il tempo).
//
// "Adatta" (fit) mostra tutta la durata nella larghezza: è anche lo zoom minimo, e resta tale se cambiano larghezza
// o durata. Lo zoom massimo è dato da setMaxPixelsPerSecond (es. 12 pixel per fotogramma).
class TimelineZoom
{
public:
    void setDuration(double seconds);
    void setWidth(double pixels);
    void setMaxPixelsPerSecond(double pixelsPerSecond);

    double duration() const { return duration_; }
    double width() const { return width_; }

    double fitPixelsPerSecond() const;  // tutta la durata nella larghezza
    double maxPixelsPerSecond() const;  // mai meno di fitPixelsPerSecond()
    double pixelsPerSecond() const;
    bool isFit() const;

    double offset() const;           // secondo al bordo sinistro
    double maxOffset() const;        // oltre, si vedrebbe dopo la fine
    double visibleSeconds() const;   // quanti secondi stanno nella larghezza
    double contentWidth() const;     // larghezza in pixel di tutta la durata a questo zoom

    double xForTime(double seconds) const;  // può uscire da [0, width] se il tempo non è visibile
    double timeForX(double x) const;        // sempre dentro [0, durata]

    // Moltiplica i pixel per secondo per `factor` tenendo fermo il tempo che sta in `x` (es. sotto il mouse).
    // Sotto lo zoom minimo diventa "Adatta", sopra il massimo si ferma al massimo.
    void zoomAt(double factor, double x);
    void fit();
    void scrollTo(double offsetSeconds);  // limitato a [0, maxOffset]
    void scrollBy(double pixels);

    // Riproduzione: se `seconds` non è visibile, si passa alla "pagina" che lo contiene (il tempo finisce poco dopo
    // il bordo sinistro), così la vista salta di una larghezza alla volta invece di scorrere a ogni fotogramma.
    // true se la vista è cambiata.
    bool follow(double seconds);

private:
    double duration_ = 0.0;
    double width_ = 1.0;
    double maxPixelsPerSecond_ = 300.0;  // 12 pixel per fotogramma a 25 fps
    bool fit_ = true;
    double pixelsPerSecond_ = 0.0;  // quando non è "Adatta"
    double offset_ = 0.0;
};

}  // namespace editor::core
