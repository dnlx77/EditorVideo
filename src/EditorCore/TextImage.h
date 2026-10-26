#pragma once

#include "EditorCore/TextTracks.h"

#include <cstdint>
#include <vector>

namespace editor::core {

// L'immagine di una scritta già disegnata: RGBA a 8 bit per canale con il colore già moltiplicato per la trasparenza
// (premoltiplicato), righe una dopo l'altra senza spazi. Il riquadro è grande quanto serve al testo (con contorno, ombra
// e sfondo); `x` e `y` sono l'angolo in alto a sinistra del riquadro nel fotogramma (possono essere anche fuori).
struct TextImage
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, premoltiplicato

    bool empty() const { return width <= 0 || height <= 0; }
};

// Chi disegna le scritte. L'implementazione con Qt sta nell'app; il nucleo (anteprima e esportazione) la usa soltanto
// attraverso questa interfaccia. `frameWidth` e `frameHeight` sono le misure del fotogramma su cui la scritta andrà.
class TextRenderer
{
public:
    virtual ~TextRenderer() = default;
    virtual TextImage render(const TextClip& clip, int frameWidth, int frameHeight) const = 0;
};

// Sovrappone la scritta a un fotogramma RGBA a 8 bit (non premoltiplicato, righe di `stride` byte, `frameWidth` x
// `frameHeight`): la parte della scritta fuori dal fotogramma si scarta. Il risultato resta con il canale alfa del
// fotogramma com'era (di solito 255).
void blendTextOver(std::uint8_t* frameRgba, int stride, int frameWidth, int frameHeight, const TextImage& text);

}  // namespace editor::core
