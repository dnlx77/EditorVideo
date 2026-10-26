#pragma once

#include "EditorApp/QtTextRenderer.h"
#include "EditorCore/TextTracks.h"

#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <vector>

namespace editor::app {

// Disegna le scritte visibili sopra l'immagine del monitor del montaggio, nell'ordine dei livelli (l'ultima è la più
// alta). Le disegna alla misura con cui si vedono sullo schermo e tiene l'ultimo risultato: finché le scritte e la misura
// non cambiano, ogni ridisegno è solo una copia dell'immagine.
class TextOverlay
{
public:
    // `target` è il rettangolo in cui sta l'immagine del video (in coordinate del `painter`); `devicePixelRatio` quello
    // dello schermo.
    void paint(QPainter& painter, const QRectF& target, qreal devicePixelRatio, const std::vector<core::TextClip>& clips);

    // Il riquadro della scritta in frazioni del fotogramma (0..1), per sapere se un clic la prende. Vuoto se non ha niente da vedere.
    QRectF bounds(const core::TextClip& clip, int frameWidth, int frameHeight) const;

    // Quante volte le scritte sono state ridisegnate da capo (per le prove).
    int renderCount() const { return renderCount_; }

private:
    QtTextRenderer renderer_;
    std::vector<core::TextClip> cachedClips_;
    QSize cachedSize_;
    QImage cached_;
    int renderCount_ = 0;
};

}  // namespace editor::app
