#pragma once

#include "EditorCore/MediaError.h"

#include <functional>
#include <string>

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
};

// Avanzamento da 0 a 1; restituire false per annullare.
using ExportProgress = std::function<bool(double fraction)>;

// Copia il segmento [inSeconds, outSeconds) di `inputPath` in `outputPath` senza ricodificare (stessi codec).
// Il contenitore si ricava dall'estensione di `outputPath`. Si copiano video e audio (non sottotitoli e dati).
// Il video parte dal fotogramma chiave in `inSeconds` (o dal primo dopo); l'audio dal primo pacchetto da lì in poi;
// i tempi vengono riportati a zero mantenendo la sincronia. I percorsi sono in UTF-8.
// Lancia MediaError (messaggio in italiano) se qualcosa non va, ExportCancelled se annullato: in entrambi i casi
// non resta un file a metà.
ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, double inSeconds,
                           double outSeconds, const ExportProgress& progress = {});

// Come sopra, con IN e OUT presi dal segmento: entrambi devono essere stati segnati.
ExportResult exportSegment(const std::string& inputPath, const std::string& outputPath, const CutSegment& segment,
                           const ExportProgress& progress = {});

}  // namespace editor::core
