#pragma once

#include "EditorCore/Encoder.h"
#include "EditorCore/MediaError.h"
#include "EditorCore/Project.h"
#include "EditorCore/TimeUtil.h"

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

// I pezzi non si possono unire in un file (né copiandoli né ricodificandoli), con il motivo in italiano.
class ClipsNotJoinable : public MediaError
{
public:
    using MediaError::MediaError;
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
    // I pezzi hanno parametri di codifica diversi (H.264/HEVC): ripetuti nel flusso davanti a ogni fotogramma chiave.
    bool parametersInStream = false;
    int reencodedSegments = 0;  // tratti ricodificati (gli altri sono copiati)
    int preciseCuts = 0;        // bordi ricodificati per il taglio preciso (da IN al fotogramma chiave, ...)
};

// Ricodifica all'esportazione (vedi exportClips).
struct ReencodeOptions
{
    QualityPreset quality = QualityPreset::High;
    bool all = false;  // "Ricodifica tutto": anche i pezzi che si potrebbero copiare
};

// Che cosa farà l'esportazione, senza farla: quanti tratti si copiano e quanti si ricodificano (e quanto durano), o
// perché non si può esportare.
struct ExportPreview
{
    int copiedSegments = 0;
    int reencodedSegments = 0;
    double copiedSeconds = 0.0;
    double reencodedSeconds = 0.0;
    // Taglio preciso: IN e OUT che non cadono su un fotogramma chiave. Quanti bordi si ricodificano e quanto durano.
    int preciseCuts = 0;
    double preciseSeconds = 0.0;
    // Il taglio preciso servirebbe ma non si può (nessun encoder per il formato del progetto): IN va al fotogramma chiave
    // prima, come senza ricodifica. Il motivo, in italiano; vuoto = nessun problema.
    std::string impreciseCuts;
    double outputSeconds = 0.0;  // durata del file che si crea (più lunga dei tratti se IN va al fotogramma chiave prima)
    // I tratti come li esporterà davvero, uno per tratto del piano e nello stesso ordine: uguali a quelli chiesti, salvo
    // l'inizio al fotogramma chiave prima quando il taglio preciso non è possibile.
    std::vector<TimeRange> effectiveRanges;
    std::string problem;  // vuoto = si può esportare
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

// Come sopra, con i tratti di più file uniti uno dopo l'altro (anche di contenitori diversi), nel formato del progetto
// (`format`; senza, quello del primo file). Ogni file si confronta con il formato (vedi classifyClip):
// - uguale: i suoi tratti si copiano così come sono;
// - parametri di codifica H.264/HEVC diversi (es. un altro numero di fotogrammi di riferimento): si copiano, e i
//   parametri di ogni file (SPS e PPS, per HEVC anche VPS) si ripetono nel flusso davanti a ogni suo fotogramma
//   chiave; nei file MP4 il video si dichiara "avc3"/"hev1" (parametri nel flusso);
// - diverso (risoluzione, fotogrammi al secondo, codec...): i suoi tratti si ricodificano nel formato del progetto con
//   l'encoder della scheda video (vedi VideoEncoder), con bande nere se le proporzioni sono diverse e i fotogrammi
//   ripetuti o saltati per arrivare ai fotogrammi al secondo del progetto; l'audio si copia se è uguale a quello del
//   formato, altrimenti si ricodifica in AAC. Anche questi tratti hanno i parametri nel flusso.
// Taglio preciso: in un tratto copiato, se IN non cade su un fotogramma chiave la parte da IN al primo fotogramma chiave
// si ricodifica (il resto si copia); se il file ha B-frame e OUT non cade su un fotogramma chiave, anche la parte
// dall'ultimo fotogramma chiave a OUT. Se fra IN e OUT non c'è un fotogramma chiave si ricodifica tutto il tratto. Senza
// un encoder per il formato del progetto IN va al fotogramma chiave prima (ExportPreview::impreciseCuts).
// Con reencode.all si ricodifica tutto. Se tutti i file si copiano con gli stessi parametri il file creato è come
// prima ("avc1"/"hvc1"), il più compatibile. ClipsNotJoinable se non si può (es. il formato non è H.264 o HEVC, o non
// c'è un encoder).
// Il contenitore del file creato si ricava dall'estensione di outputPath (di solito quella del primo file).
// Con le modifiche all'audio (AudioEdit{} se non ce ne sono): tratti silenziati (tempi della sequenza, cioè del file
// creato) e traccia sostitutiva. Con la traccia sostitutiva l'audio dei file non si copia: il file creato ha il video
// dei file e l'audio del file sostitutivo (copiato senza ricodifica, spostato di `offset` e tagliato alla fine del
// video).
ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                         const ExportProgress& progress = {}, MuteMethod muteMethod = MuteMethod::Silence,
                         const ProjectFormat* format = nullptr, const ReencodeOptions& reencode = {});

// Che cosa farebbe exportClips con questi argomenti (legge solo le caratteristiche dei file).
ExportPreview previewExport(const std::vector<ClipRanges>& clips, const AudioEdit& audio, const ProjectFormat* format,
                            const ReencodeOptions& reencode = {});

// Problema che impedisce di usare `audioPath` come traccia sostitutiva in un file come `outputPath` (dall'estensione),
// in italiano; vuoto se va bene. Es. audio PCM in un MP4.
std::string replacementAudioProblem(const std::string& audioPath, const std::string& outputPath);

}  // namespace editor::core
