#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace editor::core {

// Stato della riproduzione in un istante, visto dal timer dell'interfaccia.
struct PlaybackState
{
    bool hasVideo = false;       // il file ha un video che si sta riproducendo
    bool videoFinished = false;  // il video ha mostrato l'ultimo fotogramma
    bool audioActive = false;    // l'audio sta uscendo dalla scheda audio
    bool audioFinished = false;  // la scheda audio ha suonato tutto
    double clock = 0.0;          // posizione corrente (secondi)
    double duration = 0.0;       // durata del file (0 = sconosciuta)
};

// true quando la riproduzione è arrivata alla fine.
//
// Video e audio, se ci sono, finiscono da soli. Se invece non c'è niente che scandisce il tempo (file solo audio
// senza una scheda audio utilizzabile) la riproduzione va avanti con l'orologio di sistema fino alla durata del file:
// non deve finire subito.
bool playbackFinished(const PlaybackState& state);

// Manda `frames` campioni (per canale) di silenzio a `write`, a blocchi di ~100 ms sempre uguali: anche un'attesa
// lunga (audio esterno che comincia molto più avanti) non occupa memoria. `write` riceve campioni interlacciati e il
// loro numero (frames * channels), e restituisce false se bisogna fermarsi; in quel caso anche la funzione restituisce
// false.
bool writeSilence(std::size_t frames, int channels, int sampleRate,
                  const std::function<bool(const std::int16_t* samples, std::size_t count)>& write);

}  // namespace editor::core
