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
    void paint(QPainter& painter, const QRectF& target, qreal devicePixelRatio, const std::vector<core::TextClip>& clips,
               double time);

    // Il riquadro della scritta in frazioni del fotogramma (0..1), per sapere se un clic la prende. Vuoto se non ha niente da vedere.
    QRectF bounds(const core::TextClip& clip, int frameWidth, int frameHeight) const;

    // Quante volte una scritta è stata disegnata da capo (per le prove).
    int renderCount() const { return renderCount_; }

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
};

}  // namespace editor::app
