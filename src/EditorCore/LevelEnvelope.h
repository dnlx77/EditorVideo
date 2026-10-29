#pragma once

#include <vector>

namespace editor::core {

// Limiti del livello (in dB): la traccia, e i punti della linea del livello. Al minimo c'è silenzio.
inline constexpr double kLevelMinDb = -60.0;
inline constexpr double kLevelMaxDb = 12.0;

// Un punto della linea del livello di un pezzo o di una clip audio: da quell'istante il volume vale `db` rispetto al
// livello della traccia; fra due punti la linea sale o scende in modo lineare (in dB), prima del primo e dopo
// l'ultimo resta al valore del punto più vicino. Serve per le sfumature (dissolvenze) e per abbassare solo una parte.
// I tempi sono quelli del FILE (non della sequenza), così la linea segue il pezzo se lo si sposta, lo si divide o se ne
// cambiano i bordi.
struct LevelPoint
{
    double time = 0.0;  // in secondi, nel file
    double db = 0.0;    // 0 = com'è; kLevelMinDb = silenzio

    bool operator==(const LevelPoint&) const = default;
};

// Mette i punti in ordine di tempo (un solo punto per istante, l'ultimo vince), scarta quelli non finiti, limita i
// tempi a >= 0 e i dB a [kLevelMinDb, kLevelMaxDb].
std::vector<LevelPoint> normalizeEnvelope(std::vector<LevelPoint> points);

// Cambiando un bordo di un pezzo o di una clip, la sfumatura attaccata al bordo lo segue: se l'ultimo punto sta sul
// bordo finale (o il primo sul bordo iniziale) tutti i punti della sfumatura (quelli con livelli diversi, dal bordo
// all'indietro) si spostano dello stesso tratto, accorciando o riallungando. I punti prima restano dove sono.
// `atStart`: bordo d'inizio; `oldEdge`/`newEdge`: il bordo nel file prima e dopo; `limit`: l'altro bordo (i punti non
// lo oltrepassano).
std::vector<LevelPoint> envelopeForTrim(std::vector<LevelPoint> points, bool atStart, double oldEdge, double newEdge, double limit);

// Il valore della linea (dB) al tempo `seconds` del file; 0 se la linea è vuota.
double envelopeDbAt(const std::vector<LevelPoint>& points, double seconds);

// Quanto moltiplica il segnale un valore della linea: 0 al minimo (silenzio), altrimenti 10^(db/20).
double envelopeGain(double db);

// Il punto della linea posto al tempo `time` (entro kEdgePointTolerance), nullptr se non c'è. Serve per il pallino della
// giunzione (Project::junctions): un punto sul bordo del pezzo o della clip.
inline constexpr double kEdgePointTolerance = 1e-4;
const LevelPoint* pointAt(const std::vector<LevelPoint>& points, double time);
// La linea con un punto a `time` di livello `db` (se c'era già lì, ne cambia il livello), e senza.
std::vector<LevelPoint> withPointAt(std::vector<LevelPoint> points, double time, double db);
std::vector<LevelPoint> withoutPointAt(std::vector<LevelPoint> points, double time);

}  // namespace editor::core
