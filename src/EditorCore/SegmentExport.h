#pragma once

#include "EditorCore/MediaError.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentList.h"

#include <functional>
#include <string>
#include <vector>

namespace editor::core {

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
    // Audio nei tratti silenziati: pacchetti col contenuto sostituito dal silenzio (o PCM azzerato) e pacchetti tolti.
    int silencedPackets = 0;
    int droppedPackets = 0;
};

// Come togliere l'audio nei tratti silenziati, per l'audio compresso (il PCM viene sempre azzerato al campione).
enum class MuteMethod
{
    // Ogni pacchetto del tratto prende il contenuto di un blocco di silenzio codificato con gli stessi parametri del
    // flusso (stesso codec, frequenza, canali e profilo): tempi e durata restano quelli originali, l'audio resta
    // continuo e la sincronia non dipende dal lettore. Si sa fare per AAC-LC, MP3, MP2, AC-3 ed E-AC-3; per gli altri
    // codec (es. Opus, Vorbis) i pacchetti vengono tolti come con Omit.
    Silence,
    // I pacchetti del tratto vengono tolti: nel flusso audio resta un buco. Solo per confronto: in MP4 i lettori che
    // non guardano i tempi (es. quello di Windows) riproducono l'audio dopo il buco in anticipo.
    Omit,
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
// Con le modifiche all'audio (AudioEdit{} se non ce ne sono): tratti silenziati (tempi della sequenza, cioè del file
// creato) e traccia sostitutiva. Con la traccia sostitutiva l'audio dei file non si copia: il file creato ha il video
// dei file e l'audio del file sostitutivo (copiato senza ricodifica, spostato di `offset` e tagliato alla fine del
// video).
ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                         const ExportProgress& progress = {}, MuteMethod muteMethod = MuteMethod::Silence);

// Problema che impedisce di usare `audioPath` come traccia sostitutiva in un file come `outputPath` (dall'estensione),
// in italiano; vuoto se va bene. Es. audio PCM in un MP4.
std::string replacementAudioProblem(const std::string& audioPath, const std::string& outputPath);

}  // namespace editor::core
