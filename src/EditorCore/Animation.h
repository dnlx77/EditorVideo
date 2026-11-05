#pragma once

#include "EditorCore/Transform.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace editor::core {

// Animazione della trasformazione di un pezzo con fotogrammi chiave. (Attenzione al nome: nel resto del programma
// "keyframe" sono i fotogrammi I del video; qui si parla di "chiavi di animazione", AnimKey.)
//
// Ogni parametro animabile ha una lista di chiavi (tempo, valore, tipo di passaggio). Senza chiavi il parametro resta quello
// fisso della Transform del pezzo; con una sola chiave vale quel valore dappertutto; con più chiavi fra una e la successiva
// il valore si interpola, prima della prima e dopo l'ultima resta il valore della chiave più vicina.
//
// IL TEMPO delle chiavi è nei tempi del FILE di origine (come la linea del livello audio): così seguono il pezzo se si sposta,
// rifilando restano attaccate all'immagine e dividendo il pezzo ognuna delle due parti tiene le stesse chiavi (e il risultato
// è identico a prima della divisione, curve comprese). Le chiavi fuori dal pezzo restano memorizzate ma non si vedono.

enum class AnimParam : int
{
    ScaleX,
    ScaleY,
    PosX,
    PosY,
    Rotation,
    CropLeft,
    CropRight,
    CropTop,
    CropBottom,
    Opacity,
    Count
};
inline constexpr std::size_t kAnimParamCount = static_cast<std::size_t>(AnimParam::Count);

enum class Interp : int
{
    Linear,  // velocità costante fino alla chiave successiva
    Hold,    // il valore resta fino alla chiave successiva, poi salta
    Smooth,  // parte e arriva piano ("ease in/out"): una Bézier fissa (0.42, 0, 0.58, 1)
    Bezier,  // curva di velocità a scelta: punti di controllo (c1x, c1y), (c2x, c2y) come in CSS cubic-bezier
};

// Una chiave. `interp` e i punti di controllo valgono per il tratto che va da questa chiave alla successiva.
// Bézier: x è la frazione di tempo del tratto (0..1), y la frazione del cambio di valore (può uscire da 0..1: rimbalzo).
struct AnimKey
{
    double time = 0.0;   // secondi nel file di origine
    double value = 0.0;  // nell'unità del parametro (scala 1 = 100%, rotazione in gradi, ritaglio e opacità 0..1)
    Interp interp = Interp::Linear;
    double c1x = 0.25, c1y = 0.1, c2x = 0.25, c2y = 1.0;  // usati solo con Interp::Bezier

    bool operator==(const AnimKey&) const = default;
};

struct TransformAnimation
{
    std::array<std::vector<AnimKey>, kAnimParamCount> tracks;

    std::vector<AnimKey>& track(AnimParam p) { return tracks[static_cast<std::size_t>(p)]; }
    const std::vector<AnimKey>& track(AnimParam p) const { return tracks[static_cast<std::size_t>(p)]; }
    bool animated(AnimParam p) const { return !track(p).empty(); }
    bool empty() const;  // nessun parametro animato
    bool operator==(const TransformAnimation&) const = default;
};

// Tempi più vicini di così sono lo stesso istante.
inline constexpr double kAnimTimeEpsilon = 1e-6;

double paramValue(const Transform& t, AnimParam p);
void setParamValue(Transform& t, AnimParam p, double value);
const char* animParamName(AnimParam p);  // nome italiano ("Scala X", "Posizione Y"...)

// Il valore del tratto all'istante `time` (chiavi in ordine di tempo); `fallback` se non ci sono chiavi.
double evaluateTrack(const std::vector<AnimKey>& keys, double time, double fallback);
// Frazione di cambio (0 = valore della chiave, 1 = valore della successiva) a metà `u` (0..1) del tratto, secondo `interp`.
double easedFraction(const AnimKey& from, double u);

// La trasformazione all'istante `time` del file: `base` con i parametri animati sostituiti dal loro valore. Normalizzata.
Transform evaluateTransform(const Transform& base, const TransformAnimation& animation, double time);

// Sistema l'animazione: chiavi in ordine, senza tempi doppi (vince l'ultima), valori e tempi finiti, punti Bézier con x in 0..1.
void normalizeAnimation(TransformAnimation& animation);

// Modifiche. Tutte restituiscono true se è cambiato qualcosa.
// Mette la chiave (se a quel tempo ce n'è già una la sostituisce).
bool setKey(TransformAnimation& animation, AnimParam param, AnimKey key);
bool removeKey(TransformAnimation& animation, AnimParam param, double time);
// Sposta la chiave da `fromTime` a `toTime`; false se non c'è o se lì ce n'è già un'altra.
bool moveKey(TransformAnimation& animation, AnimParam param, double fromTime, double toTime);
// Cambia il tipo di passaggio della chiave (e i punti di controllo, se Bézier).
bool setKeyInterp(TransformAnimation& animation, AnimParam param, double time, Interp interp, double c1x = 0.25, double c1y = 0.1,
                  double c2x = 0.25, double c2y = 1.0);
// Spegne l'animazione del parametro (le chiavi spariscono).
bool clearTrack(TransformAnimation& animation, AnimParam param);
const AnimKey* findKey(const TransformAnimation& animation, AnimParam param, double time);

// Le stesse modifiche su TUTTI i parametri che hanno una chiave a quell'istante (la riga dei rombi della linea del tempo ne
// mostra uno solo per istante). moveKeysAt non fa niente se in `toTime` c'è già una chiave di uno di quei parametri.
bool moveKeysAt(TransformAnimation& animation, double fromTime, double toTime);
// Somma `delta` ai tempi di tutte le chiavi (l'ordine non cambia). Serve alle scritte, i cui tempi sono relativi all'inizio
// della scritta: accorciandone o allungandone l'inizio, o dividendola, le chiavi seguono l'immagine.
void shiftKeys(TransformAnimation& animation, double delta);
// Se `base` c'è e un parametro resta senza chiavi, prende il valore della chiave tolta (come col rombo del pannello).
bool removeKeysAt(TransformAnimation& animation, double time, Transform* base = nullptr);
bool setKeysInterpAt(TransformAnimation& animation, double time, Interp interp, double c1x = 0.25, double c1y = 0.1,
                     double c2x = 0.25, double c2y = 1.0);
// Il tipo di passaggio delle chiavi a quell'istante (quello della prima che c'è; Linear se non ce ne sono).
Interp interpAt(const TransformAnimation& animation, double time);
// I valori da dove l'animazione parte a dove arriva, uno per riga e in italiano, per il suggerimento del badge: "Scala 100%→130%".
std::vector<std::string> describeAnimation(const Transform& base, const TransformAnimation& animation);

// ---- Preset: animazioni pronte che mettono le chiavi di un pezzo che va da `from` a `to` nel file.
enum class AnimPreset : int
{
    ZoomIn,    // ingrandisce piano piano (100% → 115% della scala di base)
    ZoomOut,   // rimpicciolisce piano piano (115% → 100%)
    PanLeft,   // panoramica verso sinistra: il pezzo è ingrandito al 115% e l'immagine scorre da sinistra a destra
    PanRight,  // panoramica verso destra
    FadeIn,    // dissolvenza in apertura (opacità da 0 al valore di base, al massimo 1 secondo)
    FadeOut,   // dissolvenza in chiusura
    Count
};
inline constexpr int kAnimPresetCount = static_cast<int>(AnimPreset::Count);
const char* animPresetName(AnimPreset preset);  // nome italiano per il menu
// Applica il preset: sostituisce l'animazione dei gruppi che usa (zoom: scala; panoramica: scala fissa e posizione X; dissolvenze:
// solo il tratto iniziale o finale dell'opacità, quindi apertura e chiusura si possono avere insieme). `base` può cambiare (la
// panoramica ingrandisce il pezzo). `projectWidth` serve alla panoramica (quanto scorre l'immagine). false se il pezzo è troppo corto.
bool applyPreset(TransformAnimation& animation, Transform& base, AnimPreset preset, double from, double to, double projectWidth);

// Curve pronte per i passaggi (come cubic-bezier di CSS): per il menu del clic destro e per l'editor delle curve.
struct BezierPreset
{
    const char* name;  // italiano
    double c1x, c1y, c2x, c2y;
};
const std::vector<BezierPreset>& bezierPresets();

// Gli istanti in cui c'è almeno una chiave (di qualsiasi parametro), in ordine e senza doppi: per la riga dei rombi.
std::vector<double> keyTimes(const TransformAnimation& animation);

// ---- Gruppi di parametri, come li vede il pannello Proprietà: un rombo per gruppo, che anima insieme i suoi parametri.

enum class AnimGroup : int
{
    Scale,     // ScaleX e ScaleY
    Position,  // PosX e PosY
    Rotation,
    Crop,  // i quattro lati
    Opacity,
};
inline constexpr int kAnimGroupCount = 5;
std::vector<AnimParam> groupParams(AnimGroup group);
// Le stesse operazioni su un solo gruppo di parametri (la riga dei rombi della linea del tempo ha una sotto-riga per gruppo).
bool moveGroupKeys(TransformAnimation& animation, AnimGroup group, double fromTime, double toTime);
bool removeGroupKeys(TransformAnimation& animation, AnimGroup group, double time, Transform* base = nullptr);
bool setGroupKeysInterp(TransformAnimation& animation, AnimGroup group, double time, Interp interp, double c1x = 0.25, double c1y = 0.1,
                        double c2x = 0.25, double c2y = 1.0);
Interp groupInterpAt(const TransformAnimation& animation, AnimGroup group, double time);
// La prima chiave del gruppo a quell'istante (per il tipo di passaggio e i punti della curva); nullptr se non ce n'è.
const AnimKey* firstGroupKey(const TransformAnimation& animation, AnimGroup group, double time);
std::vector<double> groupKeyTimes(const TransformAnimation& animation, AnimGroup group);

// Quanto vicino al cursore una chiave conta "sul cursore" (il cursore si ferma sui fotogrammi: non è mai preciso al microsecondo).
inline constexpr double kAnimCursorTolerance = 0.002;

// Lo stato di un gruppo al tempo `time` del file, per un pezzo che va da `from` a `to` nel file.
struct GroupState
{
    bool animated = false;      // almeno un parametro del gruppo ha chiavi
    bool keyAtCursor = false;   // c'è una chiave sul cursore
    bool hasPrev = false;       // una chiave nel pezzo prima del cursore
    bool hasNext = false;       // una chiave nel pezzo dopo il cursore
    double prevTime = 0.0;
    double nextTime = 0.0;
    int keyCount = 0;           // quanti istanti con chiave (nel gruppo)
};
GroupState groupState(const TransformAnimation& animation, AnimGroup group, double time, double from, double to);

// Il rombo del pannello: gruppo non animato → parte l'animazione con una chiave sul cursore (valore com'è ora); animato e con
// una chiave sul cursore → la toglie (se era l'ultima, l'animazione finisce e il valore resta quello della chiave, in `base`);
// animato e senza chiave sul cursore → ne mette una (valore interpolato di adesso). `base` è la trasformazione fissa del pezzo.
// Restituisce true (cambia sempre qualcosa).
bool toggleGroupKey(TransformAnimation& animation, Transform& base, AnimGroup group, double time);

// Ripristina il gruppo: niente animazione e valori di base.
void resetGroup(TransformAnimation& animation, Transform& base, AnimGroup group);

// Auto-key: l'utente ha cambiato i valori mostrati (`shown`, valutati al tempo `time`) in `edited`. I parametri animati che
// sono cambiati ricevono una chiave sul cursore (o aggiornano quella che c'è); tutto il resto (parametri non animati,
// specchio) cambia nella trasformazione fissa `base`. Restituisce true se è cambiato qualcosa.
bool applyEditWithKeys(TransformAnimation& animation, Transform& base, const Transform& shown, const Transform& edited, double time);

}  // namespace editor::core
