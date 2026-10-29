#pragma once

#include "EditorCore/Encoder.h"
#include "EditorCore/MediaError.h"
#include "EditorCore/Project.h"
#include "EditorCore/TextImage.h"
#include "EditorCore/TimeUtil.h"
#include "EditorCore/TransitionRender.h"

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
    int blackGaps = 0;          // spazi vuoti diventati nero e silenzio
    int textSegments = 0;       // parti ricodificate perché sotto una scritta
    int transitionSegments = 0;  // parti ricodificate perché dentro una transizione
    int mixedAudioFiles = 0;    // file audio mixati e ricodificati in AAC (AudioEdit::mix); 0 = audio copiato
};

// Una transizione da fondere nel file esportato (vedi ReencodeOptions::transitions): sta sulla giunzione fra il tratto
// `beforeRange` del piano e il successivo.
struct ExportTransition
{
    int beforeRange = 0;  // posizione del tratto che finisce nella giunzione, contando tutti i tratti di tutti i file del piano
    double before = 0.0;  // secondi di transizione prima della giunzione (alla fine del tratto) e dopo (all'inizio del successivo)
    double after = 0.0;
    TransitionJob job;    // i due pezzi e l'effetto, come per l'anteprima
};

// Le transizioni del progetto per l'esportazione, nello stesso ordine dei tratti di Project::exportPlan.
std::vector<ExportTransition> exportTransitionsOf(const Project& project);

// Ricodifica all'esportazione (vedi exportClips).
struct ReencodeOptions
{
    QualityPreset quality = QualityPreset::High;
    bool all = false;  // "Ricodifica tutto": anche i pezzi che si potrebbero copiare
    bool closeGaps = false;  // "Chiudi gli spazi": i pezzi uno dopo l'altro invece del nero negli spazi vuoti
    // Scritte: sono nei tempi del montaggio (spazi vuoti compresi). I tratti sotto una scritta si ricodificano con il
    // testo disegnato (da `textRenderer`, alla misura del fotogramma del progetto) sopra a ogni fotogramma, anche nel nero
    // degli spazi vuoti; gli altri si copiano come sempre. Senza `textRenderer` le scritte non si esportano.
    TextTracks texts;
    const TextRenderer* textRenderer = nullptr;

    // Transizioni: la parte dei due tratti dentro la transizione si ricodifica, con i due pezzi decodificati e fusi
    // dall'effetto (alla misura del fotogramma del progetto, bande nere comprese); il resto come sempre. Il video
    // dei file originali si usa sempre, mai il proxy.
    std::vector<ExportTransition> transitions;

    const TextTracks* activeTexts() const { return textRenderer != nullptr && texts.endTime() > 0.0 ? &texts : nullptr; }
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
    // Tratti sotto una scritta (ricodificati con il testo sopra) e quanto durano; non contano nei bordi del taglio preciso.
    int textSegments = 0;
    double textSeconds = 0.0;
    // Parti dentro una transizione (ricodificate con i due pezzi fusi) e quanto durano; come le scritte, non contano nei
    // bordi del taglio preciso.
    int transitionSegments = 0;
    double transitionSeconds = 0.0;
    // Spazi vuoti del montaggio (ClipRanges::gapsBefore) che diventano nero e silenzio, e quanto durano.
    int blackGaps = 0;
    double blackSeconds = 0.0;
    // Gli spazi vuoti ci sono ma si chiudono perché il nero non si sa fare (formato del progetto che non si sa
    // ricodificare): il motivo, in italiano; vuoto = nessun problema (o chiusi per scelta, ReencodeOptions::closeGaps).
    std::string closedGaps;
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

// I tratti `clips` (di uno o più file, anche di contenitori diversi) uniti uno dopo l'altro e nell'ordine dato in un
// unico file `outputPath`, nel formato del progetto (`format`; senza, quello del primo file). I percorsi sono in UTF-8.
// Si copiano video e audio (non sottotitoli e dati). Nel file creato i tratti si susseguono senza buchi né
// sovrapposizioni: ognuno viene spostato con lo stesso valore per video e audio, così la sincronia si ristabilisce a
// ogni giunzione (al massimo un pacchetto audio di silenzio, ~20 ms, a ogni giunzione). Lancia MediaError (messaggio
// in italiano) se qualcosa non va, ExportCancelled se annullato: in entrambi i casi non resta un file a metà.
// Ogni file si confronta con il formato (vedi classifyClip):
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
// Spazi vuoti (ClipRanges::gapsBefore, anche prima del primo tratto): diventano nero e silenzio nel formato del
// progetto, lunghi quanto nel montaggio (arrotondati ai fotogrammi), salvo reencode.closeGaps; se il formato non si sa
// ricodificare si chiudono (ExportPreview::closedGaps). I tempi di `audio.muted` sono quelli del montaggio, spazi compresi.
// Con reencode.all si ricodifica tutto. Se tutti i file si copiano con gli stessi parametri il file creato è come
// prima ("avc1"/"hvc1"), il più compatibile. ClipsNotJoinable se non si può (es. il formato non è H.264 o HEVC, o non
// c'è un encoder).
// Il contenitore del file creato si ricava dall'estensione di outputPath (di solito quella del primo file).
// Con le modifiche all'audio (AudioEdit{} se non ce ne sono): tratti silenziati (tempi della sequenza, cioè del file
// creato) e traccia sostitutiva. Con la traccia sostitutiva l'audio dei file non si copia: il file creato ha il video
// dei file e l'audio del file sostitutivo (copiato senza ricodifica, spostato di `offset` e tagliato alla fine del
// video).
// Con AudioEdit::mix l'audio è il mix delle tracce audio (vedi MixExport.h), scritto prima in un file temporaneo
// (AAC 256 kb/s) che poi prende il posto dell'audio dei file come la traccia sostitutiva; il file creato ha il video dei
// file e quell'audio. Il 15% iniziale dell'avanzamento è per il mix.
ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                         const ExportProgress& progress = {}, MuteMethod muteMethod = MuteMethod::Silence,
                         const ProjectFormat* format = nullptr, const ReencodeOptions& reencode = {});

// Che cosa farebbe exportClips con questi argomenti (legge solo le caratteristiche dei file).
ExportPreview previewExport(const std::vector<ClipRanges>& clips, const AudioEdit& audio, const ProjectFormat* format,
                            const ReencodeOptions& reencode = {});

// Problema che impedisce di usare `audioPath` come traccia sostitutiva in un file come `outputPath` (dall'estensione),
// in italiano; vuoto se va bene. Es. audio PCM in un MP4. Se `durationSeconds` c'è, ci mette la durata dell'audio
// (0 se non la dichiara), così il file non va riaperto solo per quella.
std::string replacementAudioProblem(const std::string& audioPath, const std::string& outputPath,
                                    double* durationSeconds = nullptr);

}  // namespace editor::core
