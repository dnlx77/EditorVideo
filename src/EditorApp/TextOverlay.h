#pragma once

#include "EditorApp/QtTextRenderer.h"
#include "EditorCore/TextTracks.h"

#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <map>
#include <vector>

namespace editor::app {

// Disegna le scritte visibili sopra l'immagine del monitor del montaggio, nell'ordine dei livelli (l'ultima è la più
// alta). Ogni scritta si disegna una volta sola alla misura con cui si vede sullo schermo e si tiene l'immagine: finché
// testo, stile e misura non cambiano, ogni ridisegno è solo una copia (anche durante una dissolvenza, che cambia soltanto
// l'opacità con cui si copia).
class TextOverlay
{
public:
    // `target` è il rettangolo in cui sta l'immagine del video (in coordinate del `painter`); `devicePixelRatio` quello
    // dello schermo; `time` il tempo del montaggio (per le dissolvenze).
    // `projectFrame`: la misura in pixel del fotogramma del progetto (lo spostamento delle trasformazioni è in quei pixel); la
    // trasformazione, anche animata, si applica all'immagine disegnata intorno al centro del riquadro del testo.
    void paint(QPainter& painter, const QRectF& target, qreal devicePixelRatio, const std::vector<core::TextClip>& clips,
               double time, QSize projectFrame = {});

    // Il riquadro della scritta in frazioni del fotogramma (0..1), per sapere se un clic la prende. Vuoto se non ha niente da vedere.
    QRectF bounds(const core::TextClip& clip, int frameWidth, int frameHeight) const;

private:
    struct Entry
    {
        core::TextClip clip;  // com'era al disegno, senza posizione, durata e dissolvenze (non cambiano l'immagine)
        QSize size;
        QPoint origin;  // angolo in alto a sinistra del riquadro nel fotogramma, in pixel
        QImage image;
    };

    QtTextRenderer renderer_;
    std::map<int, Entry> entries_;  // per id di scritta
    int renderCount_ = 0;

private:
    // Solo per le prove (EditorVideoTests.exe e --smoke-test), che li raggiungono attraverso TestAccess (TestAccess.h).
    friend class TestAccess;
    // Quante volte una scritta è stata disegnata da capo.
    int renderCount() const { return renderCount_; }
};

}  // namespace editor::app
