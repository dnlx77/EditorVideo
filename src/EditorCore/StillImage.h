#pragma once

#include "EditorCore/VideoDecoder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace editor::core {

// Immagini fisse (PNG, JPG, BMP, WebP, TIFF, GIF: di una GIF si usa il primo quadro) come file del progetto. Un'immagine non ha audio, fotogrammi chiave né una
// durata propria: nel montaggio è un pezzo come gli altri e il suo fotogramma è sempre lo stesso, a qualsiasi tempo.

// Quanto "dura" un'immagine nel file, cioè fin dove si può allungare un pezzo (un'ora).
inline constexpr double kStillDuration = 3600.0;
// Da dove inizia il pezzo quando un'immagine entra nel montaggio (a metà della sua durata nel file): così il pezzo si può
// allungare sia a destra sia a sinistra.
inline constexpr double kStillCenter = kStillDuration / 2.0;
// Quanto è lungo il pezzo quando un'immagine entra nel montaggio.
inline constexpr double kStillDefaultLength = 5.0;

// Limiti di memoria: un'immagine con più pixel di kMaxStillSourcePixels non si apre; una con più di kMaxStillPixels si
// legge rimpicciolita (le proporzioni restano) a quella misura, se non si chiede un riquadro più piccolo.
inline constexpr std::int64_t kMaxStillSourcePixels = 100'000'000;
inline constexpr std::int64_t kMaxStillPixels = 24'000'000;

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

// Un lettore di riserva per i formati che l'FFmpeg di questa build non sa leggere (es. il PNG, che vuole zlib): riceve il
// percorso e restituisce l'immagine intera già orientata (BGRA non premoltiplicato), nullptr se non la legge. L'applicazione
// lo imposta all'avvio con Qt (vedi QtImageLoader.h); senza, si usa solo FFmpeg.
using ImageLoader = std::function<std::shared_ptr<VideoFrame>(const std::string& utf8Path)>;
void setImageLoader(ImageLoader loader);

// Legge l'immagine in un fotogramma BGRA (trasparenza non premoltiplicata, come i fotogrammi dei video), con l'orientamento
// EXIF applicato; time = 0. Se maxWidth e maxHeight sono > 0 e l'immagine è più grande si rimpicciolisce (proporzioni
// mantenute) per stare in quel riquadro; mai ingrandita. nullptr se il file non si apre o non è un'immagine.
std::shared_ptr<VideoFrame> loadImage(const std::string& utf8Path, int maxWidth = 0, int maxHeight = 0);

}  // namespace editor::core
