#pragma once

#include "EditorCore/MediaError.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentList.h"

#include <functional>
#include <string>
#include <vector>

namespace editor::core {

class CutSegment;

// Esportazione interrotta dall'utente (il file parziale viene cancellato).
class ExportCancelled : public MediaError
{
public:
    ExportCancelled();
};

struct ExportResult
{
    double durationSeconds = 0.0;  // durata del file creato
    int streamCount = 0;           // flussi copiati (video + audio)
    int segmentCount = 0;          // tratti uniti nel file
    int clipCount = 0;             // file di origine
};

// Avanzamento da 0 a 1; restituire false per annullare.
using ExportProgress = std::function<bool(double fraction)>;

// Copia i tratti `ranges` di `inputPath`, uno dopo l'altro e nell'ordine dato, in un unico file `outputPath`,
// senza ricodificare (stessi codec). Il contenitore si ricava dall'estensione di `outputPath`. Si copiano video e
// audio (non sottotitoli e dati). I percorsi sono in UTF-8.
//
// Ogni tratto: il video parte dal fotogramma chiave in `start` (o dal primo dopo), l'audio dal primo pacchetto da lì
// in poi e fino all'ultimo che finisce entro `end`. Nel file creato i tratti si susseguono senza buchi né
// sovrapposizioni: ognuno viene spostato con lo stesso valore per video e audio, così la sincronia si ristabilisce
// a ogni giunzione (al massimo un pacchetto audio di silenzio, ~20 ms, a ogni giunzione).
//
// Lancia MediaError (messaggio in italiano) se qualcosa non va, ExportCancelled se annullato: in entrambi i casi
// non resta un file a metà.
ExportResult exportSegments(const std::string& inputPath, const std::string& outputPath,
                            const std::vector<TimeRange>& ranges, const ExportProgress& progress = {});

// Come sopra, con i tratti di più file uniti uno dopo l'altro (anche di contenitori diversi). I file devono avere
// gli stessi codec e parametri (vedi checkCompatibility), altrimenti MediaError con l'elenco delle differenze.
// Il contenitore del file creato si ricava dall'estensione di outputPath (di solito quella del primo file).
ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath,
                         const ExportProgress& progress = {});

// Un solo tratto [inSeconds, outSeconds).
ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, double inSeconds,
                           double outSeconds, const ExportProgress& progress = {});

// Un solo tratto, con IN e OUT presi dal segmento: entrambi devono essere stati segnati.
ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, const CutSegment& segment,
                           const ExportProgress& progress = {});

}  // namespace editor::core
