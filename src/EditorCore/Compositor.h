#pragma once

#include "EditorCore/Transform.h"

#include <cstdint>

namespace editor::core {

// Immagine BGRA a 8 bit (come i fotogrammi dell'anteprima), opaca: l'alfa non si legge.
struct BgraImage
{
    const std::uint8_t* data = nullptr;
    int stride = 0;  // byte per riga
    int width = 0;
    int height = 0;
};

// Disegna `source` sul fotogramma `canvas` (BGRA, opaco, della misura del progetto) con la trasformazione `transform`:
// ritaglio, scala, rotazione, specchio e posizione (vedi computeGeometry; la scala 100% è il file adattato al fotogramma),
// poi fonde con l'opacità. Quello che non è coperto dal pezzo resta com'è sul fotogramma, quindi si possono disegnare
// più livelli uno sopra l'altro (dal più basso al più alto).
// - Interpolazione bilineare; se il pezzo si rimpicciolisce molto prima lo si riduce con una media di blocchi interi,
//   per non avere righe a scalini.
// - I bordi del pezzo (ritaglio e contorno) sono sfumati di mezzo pixel, quindi non ci sono bordi a scalini nemmeno
//   ruotando; con spostamenti interi e senza rotazione i bordi cadono sui pixel e restano netti.
// `source` può avere una misura diversa da quella del file originale (es. un proxy): conta solo la sua forma.
// Immagini nulle o senza misura: non fa niente.
void compositeLayer(std::uint8_t* canvas, int canvasStride, int canvasWidth, int canvasHeight, const BgraImage& source,
                    const Transform& transform);

}  // namespace editor::core
