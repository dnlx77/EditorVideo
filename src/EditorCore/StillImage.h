#pragma once

#include "EditorCore/VideoDecoder.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace editor::core {

// Immagini fisse (PNG, JPG, BMP, WebP, TIFF...) come file del progetto. Un'immagine non ha audio, fotogrammi chiave né una
// durata propria: nel montaggio è un pezzo come gli altri e il suo fotogramma è sempre lo stesso, a qualsiasi tempo.

// Quanto "dura" un'immagine nel file, cioè fin dove si può allungare un pezzo (un'ora).
inline constexpr double kStillDuration = 3600.0;
// Quanto è lungo il pezzo quando un'immagine entra nel montaggio.
inline constexpr double kStillDefaultLength = 5.0;

// Estensioni delle immagini (minuscole, con il punto).
const std::vector<std::string>& imageExtensions();
// true se l'estensione del file è quella di un'immagine (non guarda il contenuto).
bool isImagePath(const std::string& utf8Path);

struct ImageSize
{
    int width = 0;
    int height = 0;
};

// Apre l'immagine e ne dà la misura com'è mostrata (orientamento EXIF applicato); std::nullopt se il file non esiste o
// non è un'immagine leggibile.
std::optional<ImageSize> inspectImage(const std::string& utf8Path);

// Legge l'immagine in un fotogramma BGRA (trasparenza non premoltiplicata, come i fotogrammi dei video), con l'orientamento
// EXIF applicato; time = 0. Se maxWidth e maxHeight sono > 0 e l'immagine è più grande si rimpicciolisce (proporzioni
// mantenute) per stare in quel riquadro; mai ingrandita. nullptr se il file non si apre o non è un'immagine.
std::shared_ptr<VideoFrame> loadImage(const std::string& utf8Path, int maxWidth = 0, int maxHeight = 0);

}  // namespace editor::core
