#pragma once

#include "EditorCore/MixSpec.h"
#include "EditorCore/Project.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace editor::core {

class AudioDecoder;

// I tratti da mixare per il progetto (A1 e tracce aggiunte), in ordine di posizione.
std::vector<MixSegment> mixSegmentsOf(const Project& project);
// Quanto si moltiplica ogni traccia nel mix (A1 compresa), come AudioTracks::gain.
std::map<int, float> trackGainsOf(const AudioTracks& tracks);
// Id con cui `takePeaks` dà i picchi del Master (tutto il mix, dopo il livello del Master e prima del limitatore).
inline constexpr int kMasterTrackId = AudioTracks::kMasterTrack;
// Gli effetti attivi di ogni traccia (A1 e Master compresi, con id kMasterTrackId); senza voce = nessun effetto.
std::map<int, std::vector<AudioEffect>> trackEffectsOf(const AudioTracks& tracks);
// Il pan di ogni traccia (A1 compresa), da -1 a +1.
std::map<int, float> trackPansOf(const AudioTracks& tracks);
// Tutto quello che serve per mixare il progetto (con gli spazi vuoti di A1 chiusi se `closeGaps`, come nel file esportato).
MixSpec mixSpecOf(const Project& project, bool closeGaps = false);
// Le linee del livello dei tratti, nello stesso ordine di mixSegmentsOf (per AudioMixer::setEnvelopes).
std::vector<std::vector<LevelPoint>> segmentEnvelopesOf(const Project& project);

// Il picco (0..1, valore assoluto, sinistro o destro) di tutto il mix di `spec` come arriva al Master: dopo il suo
// livello e il suo equalizzatore, prima della normalizzazione e del limitatore. Rende tutto il mix (dal tempo 0 alla fine),
// quindi può durare; `progress` (0..1) restituisce false per annullare (allora il risultato è 0 e `cancelled` diventa true).
float measureMixPeak(const MixSpec& spec, const std::function<bool(double)>& progress = {}, bool* cancelled = nullptr);

// Il mixer della sequenza: somma, a 48 kHz stereo, l'audio di tutti i tratti presenti in ogni istante, ognuno con il
// livello e muto/solo della sua traccia, la linea del livello del tratto e senza i tratti silenziati. Un limitatore dolce evita la saturazione.
//
// render() si usa di seguito (blocchi uno dopo l'altro) da un solo thread; un salto a un altro punto va bene, costa
// solo il riposizionamento dei file. setGain() si può chiamare anche da un altro thread, mentre si suona: il livello
// cambia con una breve rampa, senza scatti.
class AudioMixer
{
public:
    AudioMixer(std::vector<MixSegment> segments, std::map<int, float> gains, std::map<int, float> pans = {},
                float masterGain = 1.0f, std::map<int, std::vector<AudioEffect>> effects = {});
    ~AudioMixer();
    AudioMixer(AudioMixer&&) noexcept;
    AudioMixer& operator=(AudioMixer&&) noexcept;

    // Fine dell'ultimo tratto, in secondi.
    double endTime() const { return end_; }

    void setGain(int trackId, float gain);  // usata dalle prove (il programma usa setGains)
    void setGains(const std::map<int, float>& gains);
    // Il pan delle tracce (-1 sinistra .. +1 destra, assente = centro); cambia con la stessa rampa del livello.
    void setPans(const std::map<int, float>& pans);
    // Il livello del Master (moltiplicatore di tutto il mix), con la stessa rampa.
    void setMasterGain(float gain);
    // Gli effetti attivi di ogni traccia (trackEffectsOf), anche mentre si suona; più equalizzatori agiscono uno dopo l'altro.
    void setEffects(std::map<int, std::vector<AudioEffect>> effects);
    // Le linee del livello dei tratti, una per tratto nello stesso ordine (al posto di quelle di prima): il valore al
    // tempo del file moltiplica il livello della traccia, campione per campione.
    void setEnvelopes(std::vector<std::vector<LevelPoint>> envelopes);
    // I picchi (valore assoluto massimo, 0..1 prima del limitatore) di ogni traccia, sinistra e destra, nei blocchi
    // resi dall'ultima chiamata; poi si azzerano. Per i vu-meter: si può chiamare da un altro thread.
    std::map<int, std::array<float, 2>> takePeaks();
    // Lo spettro: si tiene d'occhio una sola traccia (o il Master, kMasterTrackId); `kNoSpectrum` = nessuna. Dei blocchi
    // resi si conservano gli ultimi campioni (mono, dopo gli effetti della traccia; per il Master dopo il suo livello).
    static constexpr int kNoSpectrum = -1000;
    void setSpectrumWatch(int trackId);
    // Gli ultimi `count` campioni mono della traccia tenuta d'occhio, dal più vecchio; false se non ce ne sono ancora
    // abbastanza (o nessuna traccia è tenuta d'occhio). Si può chiamare da un altro thread.
    bool takeSpectrumSamples(std::vector<float>& out, std::size_t count);

    // Scrive in `out` (stereo interlacciato, `frames` * 2 campioni) l'audio dal fotogramma `firstFrame` (contato dal
    // tempo 0 della sequenza). Dove non c'è niente: silenzio.
    void render(long long firstFrame, std::size_t frames, std::vector<std::int16_t>& out);

    // Limitatore dolce: lascia com'è fino a 0.95, poi si avvicina a 1 senza superarlo.
    static float limit(float value);

private:
    struct Reader;
    struct Shared;
    struct Settings;
    // Picco sinistro e destro di una traccia; `used` = la traccia ha suonato (le voci restano, così non si alloca).
    struct TrackPeak
    {
        std::array<float, 2> value{};
        bool used = false;
    };

    // Cambia le impostazioni condivise: una copia nuova, che il thread audio prende al blocco seguente.
    void changeSettings(const std::function<void(Settings&)>& change);

    std::vector<MixSegment> segments_;
    std::vector<long long> first_;   // primo fotogramma di ogni tratto nella sequenza
    std::vector<long long> length_;  // fotogrammi di ogni tratto
    std::vector<std::unique_ptr<Reader>> readers_;
    std::unique_ptr<Shared> shared_;  // livelli (condivisi con altri thread)
    std::map<int, float> applied_;    // livello arrivato alla fine dell'ultimo blocco, per la rampa
    std::map<int, float> appliedPans_;  // lo stesso per il pan
    float appliedMaster_ = 1.0f;        // e per il Master
    struct EqState
    {
        std::vector<EqualizerFilter> filters;  // uno per ogni equalizzatore attivo della traccia, in ordine
        std::vector<float> buffer;  // la somma della traccia in questo blocco, da filtrare
    };
    std::map<int, EqState> eq_;       // un equalizzatore per traccia (e per il Master)
    long long nextExpected_ = -1;     // il fotogramma che seguirebbe l'ultimo blocco: se è un altro si è saltato
    double end_ = 0.0;
    // Riusati a ogni blocco da render, così il thread audio non alloca memoria.
    std::vector<float> acc_;                                        // somma del blocco in lavorazione
    std::map<int, TrackPeak> blockPeaks_;                           // picchi del blocco, per traccia
    std::vector<std::pair<int, std::vector<float>*>> trackBuffers_;  // tracce sommate a parte (equalizzatore, spettro)
    std::vector<std::pair<long long, long long>> mutedFrames_;      // tratti silenziati del tratto in lettura
};

}  // namespace editor::core
