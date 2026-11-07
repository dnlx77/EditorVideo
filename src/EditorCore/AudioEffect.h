#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace editor::core {

// Un effetto messo su una traccia audio (o sul Master) nel Mixer.
enum class EffectKind
{
    Equalizer,  // equalizzatore parametrico a più bande (EqBand)
    Normalize,  // solo sul Master: porta il picco di tutto il mix a un livello (AudioEffect::targetDb)
};

// Il tipo di filtro di una banda dell'equalizzatore.
enum class EqBandType
{
    LowCut,     // taglia i bassi sotto la frequenza (passa-alto)
    LowShelf,   // alza o abbassa tutto ciò che sta sotto la frequenza
    Bell,       // campana: alza o abbassa intorno alla frequenza
    HighShelf,  // alza o abbassa tutto ciò che sta sopra la frequenza
    HighCut,    // taglia gli alti sopra la frequenza (passa-basso)
};

inline constexpr double kEqMinHz = 20.0;
inline constexpr double kEqMaxHz = 20000.0;
inline constexpr double kEqMinQ = 0.1;
inline constexpr double kEqMaxQ = 10.0;
inline constexpr double kEqMaxGainDb = 18.0;  // i guadagni vanno da -18 a +18 dB
inline constexpr std::size_t kEqBandCount = 6;
inline constexpr double kNormalizeMinDb = -24.0;  // il livello di arrivo del picco va da -24 a -0,5 dB
inline constexpr double kNormalizeMaxDb = -0.5;

struct EqBand
{
    EqBandType type = EqBandType::Bell;
    bool enabled = true;
    double freq = 1000.0;   // Hz
    double gainDb = 0.0;    // non conta per i tagli
    double q = 0.71;

    bool hasGain() const { return type != EqBandType::LowCut && type != EqBandType::HighCut; }
    // Se agisce davvero: attiva e, per campana e shelf, con un guadagno che cambia qualcosa.
    bool active() const;
    bool operator==(const EqBand&) const = default;
};

// Le sei bande di partenza: taglio bassi (spento), shelf bassi, due campane, shelf alti, taglio alti (spento); tutte senza
// guadagno, quindi l'equalizzatore all'inizio non cambia il suono.
std::vector<EqBand> defaultEqBands();
EqBand clampedBand(EqBand band);

struct AudioEffect
{
    EffectKind kind = EffectKind::Equalizer;
    bool enabled = true;  // false = escluso (resta nello slot ma non agisce)
    std::vector<EqBand> bands = defaultEqBands();  // per l'equalizzatore
    double targetDb = -1.0;                        // per Normalize: il picco del mix dopo la normalizzazione

    // Se agisce davvero sull'audio: attivo e con almeno una banda che cambia qualcosa.
    bool active() const;
    bool operator==(const AudioEffect&) const = default;
};

const char* effectName(EffectKind kind);  // "Equalizzatore", "Normalizza"
// Il moltiplicatore che porta un picco misurato (0..1, valore assoluto) al livello del primo Normalize attivo della lista;
// 1 se non ce n'è (o se il mix è muto). Mai oltre +40 dB.
float normalizeGain(const std::vector<AudioEffect>& effects, float measuredPeak);
bool hasActiveNormalize(const std::vector<AudioEffect>& effects);
const char* bandTypeName(EqBandType type);
// Porta i parametri nei limiti (un equalizzatore senza bande riceve quelle di partenza).
AudioEffect clampedEffect(AudioEffect effect);

// La risposta in frequenza (dB) di una serie di bande a `hz`, alla frequenza di campionamento `sampleRate`.
double eqResponseDb(const std::vector<EqBand>& bands, double hz, int sampleRate);

// Equalizzatore parametrico (filtri biquad RBJ in forma diretta I), sinistra e destra separati. I coefficienti si
// ricalcolano ai cambi di parametri senza toccare lo stato (niente scatti); reset() azzera lo stato (dopo un salto).
class EqualizerFilter
{
public:
    void setBands(const std::vector<EqBand>& bands, int sampleRate);
    void reset();
    // Filtra `frames` fotogrammi stereo interlacciati (float) sul posto.
    void process(float* samples, std::size_t frames);

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        bool on = false;
        double x1[2] = {0, 0}, x2[2] = {0, 0}, y1[2] = {0, 0}, y2[2] = {0, 0};
    };
    std::vector<Biquad> stages_;
};

}  // namespace editor::core
