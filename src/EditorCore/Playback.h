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
// Velocità dell'anteprima scelte dal menu (1 = normale).
inline constexpr double kPlaybackSpeeds[] = {0.25, 0.5, 1.0, 1.5, 2.0, 4.0};

// Riproduzione a spola (J indietro, L avanti, premuti più volte): la velocità dopo un tasto. `current` è la velocità
// attuale (negativa = all'indietro), `playing` se si sta riproducendo, `direction` +1 (L) o -1 (J). Il primo tasto
// parte a 1x nella sua direzione; ripetuto raddoppia fino a 4x; quello opposto riparte a 1x dall'altra parte.
double shuttleSpeed(double current, bool playing, int direction);

// Se a questa velocità si sente l'audio (con la stessa intonazione): solo in avanti e fino a 2x.
bool audibleAtSpeed(double speed);

bool writeSilence(std::size_t frames, int channels, int sampleRate,
                  const std::function<bool(const std::int16_t* samples, std::size_t count)>& write);

}  // namespace editor::core
