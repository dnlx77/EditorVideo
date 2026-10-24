#pragma once

#include "EditorCore/Project.h"
#include "EditorCore/TimeUtil.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace editor::core {

class AudioDecoder;

// Frequenza e canali del mix (stereo a 48 kHz, come l'audio che si esporterà).
inline constexpr int kMixSampleRate = 48000;
inline constexpr int kMixChannels = 2;

// Un tratto di un file messo in un punto della sequenza: o l'audio di un pezzo video (traccia A1) o una clip di una
// traccia audio aggiunta.
struct MixSegment
{
    std::string path;        // UTF-8
    double fileStart = 0.0;  // nel file
    double fileEnd = 0.0;
    double position = 0.0;   // dove inizia nella sequenza
    int trackId = AudioTracks::kOriginalTrack;
    std::vector<TimeRange> muted;  // tratti senza audio, nei tempi del file

    double length() const { return fileEnd - fileStart; }
};

// I tratti da mixare per il progetto (A1 e tracce aggiunte), in ordine di posizione.
std::vector<MixSegment> mixSegmentsOf(const Project& project);
// Quanto si moltiplica ogni traccia nel mix (A1 compresa), come AudioTracks::gain.
std::map<int, float> trackGainsOf(const AudioTracks& tracks);

// Il mixer della sequenza: somma, a 48 kHz stereo, l'audio di tutti i tratti presenti in ogni istante, ognuno con il
// livello e il muto/solo della sua traccia e senza i tratti silenziati. Un limitatore dolce evita la saturazione.
//
// render() si usa di seguito (blocchi uno dopo l'altro) da un solo thread; un salto a un altro punto va bene, costa
// solo il riposizionamento dei file. setGain() si può chiamare anche da un altro thread, mentre si suona: il livello
// cambia con una breve rampa, senza scatti.
class AudioMixer
{
public:
    AudioMixer(std::vector<MixSegment> segments, std::map<int, float> gains);
    ~AudioMixer();
    AudioMixer(AudioMixer&&) noexcept;
    AudioMixer& operator=(AudioMixer&&) noexcept;

    // Fine dell'ultimo tratto, in secondi.
    double endTime() const { return end_; }

    void setGain(int trackId, float gain);
    void setGains(const std::map<int, float>& gains);

    // Scrive in `out` (stereo interlacciato, `frames` * 2 campioni) l'audio dal fotogramma `firstFrame` (contato dal
    // tempo 0 della sequenza). Dove non c'è niente: silenzio.
    void render(long long firstFrame, std::size_t frames, std::vector<std::int16_t>& out);

    // Limitatore dolce: lascia com'è fino a 0.95, poi si avvicina a 1 senza superarlo.
    static float limit(float value);

private:
    struct Reader;
    struct Shared;

    std::vector<MixSegment> segments_;
    std::vector<long long> first_;   // primo fotogramma di ogni tratto nella sequenza
    std::vector<long long> length_;  // fotogrammi di ogni tratto
    std::vector<std::unique_ptr<Reader>> readers_;
    std::unique_ptr<Shared> shared_;  // livelli (condivisi con altri thread)
    std::map<int, float> applied_;    // livello arrivato alla fine dell'ultimo blocco, per la rampa
    double end_ = 0.0;
};

}  // namespace editor::core
