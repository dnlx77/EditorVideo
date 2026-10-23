#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace editor::core {

// Cambia la velocità dell'audio senza cambiarne l'intonazione (WSOLA: pezzetti di ~30 ms sovrapposti, ognuno preso
// dall'input nel punto che si raccorda meglio con il precedente). Serve all'anteprima a velocità diverse da 1x.
//
// L'uscita corrisponde all'ingresso così: il campione n dell'uscita è il campione n * speed dell'ingresso (all'inizio
// c'è una breve dissolvenza in entrata). A velocità 1 i campioni passano così come sono.
class TimeStretch
{
public:
    // speed > 0 (es. 0.5 = metà velocità, 2 = doppia).
    TimeStretch(int channels, int sampleRate, double speed);

    double speed() const { return speed_; }

    // Campioni interlacciati (frames * channels).
    void push(const std::int16_t* samples, std::size_t count);
    // Aggiunge a `out` i campioni pronti (interlacciati).
    void pull(std::vector<std::int16_t>& out);
    // Fine dell'ingresso: aggiunge a `out` anche quello che resta.
    void flush(std::vector<std::int16_t>& out);

private:
    std::size_t inputFrames() const { return input_.size() / channels_; }
    // Inizio (nell'ingresso) del pezzo vicino a `nominal` che si raccorda meglio con la continuazione di `previous`.
    std::size_t bestStart(std::size_t nominal, std::size_t previous) const;
    void dropConsumed();

    int channels_;
    double speed_;
    std::size_t window_;  // lunghezza di un pezzo (campioni per canale)
    std::size_t hop_;     // passo nell'uscita: metà pezzo
    std::size_t search_;  // quanto si cerca intorno al punto previsto, per parte
    std::vector<float> hann_;

    std::vector<float> input_;      // ingresso non ancora consumato (interlacciato)
    std::vector<float> overlap_;    // seconda metà del pezzo precedente, già pesata, da sommare al prossimo
    double nominal_ = 0.0;          // punto previsto (nell'ingresso) del prossimo pezzo
    std::size_t previous_ = 0;      // inizio del pezzo precedente nell'ingresso
    std::size_t produced_ = 0;      // campioni (per canale) già dati in uscita
    std::size_t dropped_ = 0;       // campioni (per canale) già tolti dall'inizio di input_
    bool first_ = true;
};

}  // namespace editor::core
