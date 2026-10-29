#pragma once

#include "EditorCore/AudioTracks.h"
#include "EditorCore/Groups.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MixSpec.h"
#include "EditorCore/Sequence.h"
#include "EditorCore/TextTracks.h"
#include "EditorCore/TransitionList.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace editor::core {

// Un file di partenza del progetto.
struct Clip
{
    int id = 0;          // identificativo stabile
    std::string path;    // UTF-8
    bool analyzed = false;  // fotogrammi chiave e durata noti
    // Fotogrammi chiave in ordine. Dopo l'analisi non cambiano più: sono condivisi fra le copie del progetto (ogni
    // passo di Annulla/Ripeti è una copia, con file da migliaia di fotogrammi chiave).
    std::shared_ptr<const std::vector<double>> keyframeData;
    double duration = 0.0;

    const std::vector<double>& keyframes() const;  // vuoto se il file non è stato analizzato

    // Fotogramma chiave nello stesso punto o subito prima (prima del primo: il primo; senza: `seconds`).
    double snapToKeyframe(double seconds) const;
};

// Tratti di un file da esportare, uno dopo l'altro.
struct ClipRanges
{
    std::string path;
    std::vector<TimeRange> ranges;
    // Fotogrammi chiave del file (in ordine), per il taglio preciso; vuoto = da leggere dal file.
    std::vector<double> keyframes;
    // Spazio vuoto nel montaggio prima di ogni tratto, in secondi (stesso ordine di `ranges`; vuoto = nessuno).
    std::vector<double> gapsBefore;
    // Caratteristiche dei flussi già lette (per le anteprime, che altrimenti riaprono ogni file); nullptr = da leggere
    // dal file, come fa sempre l'esportazione vera.
    std::shared_ptr<const MediaSignature> signature;
};

// Un file audio (MP3, AAC, M4A, WAV...) che prende il posto dell'audio originale di tutta la sequenza. Nelle tracce
// audio (AudioTracks) è una clip su A2, dal punto `offset` fino alla fine del file, con A1 silenziata.
struct AudioReplacement
{
    std::string path;     // UTF-8
    double offset = 0.0;  // dove inizia nella sequenza, in secondi (negativo = se ne salta l'inizio)
    double duration = 0.0;
};

// Cosa fare dell'audio esportando la sequenza.
struct AudioEdit
{
    // Tratti senza audio, nei tempi della sequenza (lì si sente silenzio).
    std::vector<TimeRange> muted;
    // Se c'è, l'audio originale dei file viene escluso e al suo posto va questo.
    std::optional<AudioReplacement> replacement;
    // Se c'è, l'audio esportato è il mix delle tracce audio (A1 e le aggiunte, con livelli, linee del livello, muto e
    // solo), ricodificato in AAC 48 kHz stereo 256 kb/s; `muted` e `replacement` non contano. `mix` ha gli spazi vuoti
    // del montaggio (nero e silenzio), `mixClosed` li ha chiusi: vale quest'ultimo se l'esportazione chiude gli spazi.
    std::shared_ptr<const MixSpec> mix;
    std::shared_ptr<const MixSpec> mixClosed;

    bool empty() const { return muted.empty() && !replacement && !mix; }
};

// Cosa diventano gli spazi vuoti del montaggio nel file esportato: nero e silenzio (lunghi come nel montaggio,
// ricodificati nel formato del progetto), oppure si chiudono e i pezzi si uniscono uno dopo l'altro.
enum class GapExport
{
    Close,
    Black,
};

// Dove sta una transizione nel montaggio: a cavallo della giunzione fra due pezzi, `before` secondi prima e `after` dopo
// (before + after = durata). Prima della giunzione il pezzo che segue si vede con il suo materiale precedente al
// proprio inizio, dopo la giunzione il pezzo che precede si vede con quello successivo alla propria fine: servono
// margini nei file ("maniglie"), e se da un lato mancano la transizione si sposta dall'altro.
struct TransitionPlacement
{
    int id = 0;
    int beforeFragment = 0;
    int afterFragment = 0;
    double junction = 0.0;  // tempo della giunzione nella sequenza
    double before = 0.0;
    double after = 0.0;
    double start() const { return junction - before; }
    double end() const { return junction + after; }
    double duration() const { return before + after; }
};

// Quanto può estendersi una transizione su una giunzione: `before` secondi prima, `after` dopo (tutti e due 0 se lì non
// c'è una giunzione valida). La durata massima è la somma.
struct TransitionLimits
{
    double before = 0.0;
    double after = 0.0;
    double max() const { return before + after; }
};

// Durata minima di una transizione, in secondi.
inline constexpr double kMinTransition = 0.1;

// Progetto: i file di partenza e la sequenza (il montaggio) fatta con pezzi di quei file.
class Project
{
public:
    // Aggiunge un file e ne restituisce l'id. L'analisi arriva dopo con setAnalysis().
    int add(std::string path);
    void setAnalysis(int id, std::vector<double> keyframes, double duration);
    // Il file si trova in un altro posto (es. ritrovato riaprendo un progetto): l'analisi va rifatta.
    void setClipPath(int id, std::string path);
    // Toglie il file e tutti i suoi pezzi dalla sequenza.
    void remove(int id);
    void clear();

    const std::vector<Clip>& clips() const { return clips_; }
    const Clip* find(int id) const;
    int indexOf(int id) const;  // -1 se non c'è
    bool empty() const { return clips_.empty(); }
    bool allAnalyzed() const;

    Sequence& sequence() { return sequence_; }
    const Sequence& sequence() const { return sequence_; }
    // La sequenza come elenco di file e tratti da esportare (pezzi consecutivi dello stesso file insieme), con lo spazio
    // vuoto prima di ogni tratto (ClipRanges::gapsBefore; con GapExport::Close nessuno).
    std::vector<ClipRanges> exportPlan(GapExport gaps = GapExport::Close) const;

    // Tracce audio: A1 (l'audio dei pezzi video) e quelle aggiunte con le clip audio.
    AudioTracks& audioTracks() { return audio_; }
    const AudioTracks& audioTracks() const { return audio_; }

    // Livelli di scritte (titoli e testi) sopra il video.
    TextTracks& textTracks() { return text_; }
    const TextTracks& textTracks() const { return text_; }
    // Gli stili con nome delle scritte, salvati con il progetto.
    std::vector<NamedTextStyle>& textStyles() { return textStyles_; }
    const std::vector<NamedTextStyle>& textStyles() const { return textStyles_; }

    // Transizioni sulle giunzioni fra pezzi video (vedi TransitionPlacement). transitions() è l'archivio, grezzo: per
    // l'anteprima e l'esportazione serve activeTransitions(), che lascia fuori quelle che non valgono più.
    const TransitionList& transitions() const { return transitions_; }
    // Per rimettere le transizioni salvate (ProjectFile) e per le prove; le modifiche vere passano da addTransition ecc.
    TransitionList& mutableTransitions() { return transitions_; }
    // La giunzione fra il pezzo `beforeFragment` e quello che lo segue: valida se sono attaccati (senza spazio vuoto)
    // e il file del primo è analizzato (serve la durata per i margini).
    bool isJunction(int beforeFragment) const;
    // Fin dove può arrivare una transizione lì (vedi TransitionLimits), tenendo conto delle transizioni vicine.
    TransitionLimits transitionLimits(int beforeFragment) const;
    // Mette una transizione del registro sulla giunzione dopo `beforeFragment`; restituisce l'id, 0 se non si può
    // (tipo sconosciuto, nessuna giunzione, già una lì, durata fuori da kMinTransition..transitionLimits.max()).
    int addTransition(int beforeFragment, const std::string& type, double duration);
    bool removeTransition(int id);
    // Cambia la durata; false (e nessuna modifica) se fuori dai limiti.
    bool setTransitionDuration(int id, double duration);
    // Cambia tipo, impostazioni e dissolvenza dell'audio; false se la transizione o il tipo non ci sono.
    bool setTransitionSettings(int id, const std::string& type, const TransitionParams& params, bool crossfadeAudio);
    // Dove sta la transizione (std::nullopt se non vale: giunzione sparita, durata che non ci sta più, tipo sconosciuto).
    std::optional<TransitionPlacement> transitionPlacement(int id) const;
    // Tutte quelle che valgono, in ordine di tempo.
    std::vector<TransitionPlacement> activeTransitions() const;
    // Toglie le transizioni la cui giunzione non c'è più e accorcia quelle che non ci stanno più (se non resta
    // kMinTransition le toglie). Si chiama dopo le modifiche alla sequenza. Restituisce quante ne ha toccate.
    int pruneTransitions();

    // Traccia audio sostitutiva (vedi AudioReplacement), il caso più semplice delle tracce audio: impostarla toglie
    // tutte le tracce aggiunte e mette il file su A2 con A1 silenziata; std::nullopt rimette le tracce come
    // all'inizio (solo A1). audioReplacement() la ricava dalle tracce: c'è solo se sono proprio in quella forma
    // (A1 silenziata e una sola clip, su A2, fino alla fine del file, senza livelli cambiati).
    void setAudioReplacement(std::optional<AudioReplacement> replacement);
    std::optional<AudioReplacement> audioReplacement() const;
    // Tratti silenziati e traccia sostitutiva, per l'esportazione: nei tempi del file esportato (con gli spazi vuoti
    // trattati come dice `gaps`).
    AudioEdit audioEdit(GapExport gaps = GapExport::Close) const;
    // Scollega l'audio del pezzo video: diventa una clip audio libera (su una traccia aggiunta, la prima dove ci sta,
    // altrimenti una nuova) nello stesso punto, con la stessa linea del livello e gli stessi tratti muti; l'audio del
    // pezzo in A1 viene silenziato. Restituisce l'id della clip, 0 se non si può (pezzo inesistente o con l'audio già
    // tutto silenziato). *track riceve l'id della traccia.
    int unlinkAudio(int fragmentId, int* track = nullptr);
    // Il contrario, se la clip è ancora allineata al pezzo (stesso file, stesso sincronismo): toglie dalle tracce le
    // clip di quel file nel tratto del pezzo e rimette l'audio del pezzo (con la linea della clip, se era una sola).
    // false se non ce n'è nessuna allineata.
    bool relinkAudio(int fragmentId);
    // Se esportando l'audio non basta copiarlo com'è (vedi AudioTracks::needsMix) o i pezzi video hanno una linea del
    // livello: allora audioEdit() ha il mix.
    bool audioNeedsMix() const;

    // Gruppi di oggetti (pezzi video, audio dei pezzi, clip audio) che si muovono insieme. Quelli fatti dall'utente
    // stanno in groups(); in più un pezzo video e il suo audio (su A1) sono legati da quando nascono, finché l'audio non
    // viene scollegato. groupMembers dà tutti gli oggetti legati a `object` in un modo o nell'altro (lui compreso;
    // solo quelli che esistono ancora), in ordine; un oggetto senza legami dà solo se stesso.
    Groups& groups() { return groups_; }
    const Groups& groups() const { return groups_; }
    bool objectExists(ObjectRef object) const;
    std::vector<ObjectRef> groupMembers(ObjectRef object) const;
    // Mette gli oggetti (quelli che esistono) in un gruppo nuovo; restituisce l'id, 0 se non c'è niente da raggruppare
    // (meno di due oggetti, o sono già tutti legati fra loro).
    // Se fra gli oggetti ci sono un pezzo con l'audio scollegato e la sua clip audio ancora allineata, G li riunisce
    // come prima di U (relinkAudio): *relinked conta i pezzi ricollegati (in quel caso il valore restituito può essere 0).
    int groupObjects(const std::vector<ObjectRef>& objects, int* relinked = nullptr);
    // Scioglie i gruppi degli oggetti; se non ne ha sciolto nessuno, scollega l'audio dei pezzi video fra gli oggetti
    // (unlinkAudio): così con U si scioglie prima il gruppo e poi il legame fra il video e il suo audio.
    struct UngroupResult
    {
        int dissolved = 0;      // gruppi sciolti
        int audioUnlinked = 0;  // audio scollegati dal loro pezzo
    };
    UngroupResult ungroupObjects(const std::vector<ObjectRef>& objects);
    // Divide al tempo `seconds` della sequenza i pezzi video e le clip audio del gruppo di `anchor` che lo attraversano
    // (il pezzo video divide con sé il suo audio). Le seconde parti formano un gruppo nuovo, se sono almeno due oggetti.
    // Restituisce le seconde parti (pezzi e clip); vuoto, e nessuna modifica, se niente si può dividere lì.
    std::vector<ObjectRef> splitGroup(ObjectRef anchor, double seconds);
    // Toglie gli oggetti dal montaggio: i pezzi video (con il loro audio), le clip audio e, per l'audio di un pezzo
    // senza il pezzo, l'audio del pezzo (silenzio). I gruppi restano con gli oggetti rimasti. Restituisce quanti sono
    // stati tolti.
    int removeObjects(const std::vector<ObjectRef>& objects);
    // Spostamento di gruppo. Il gruppo di `anchor` (pezzi video e clip audio: l'audio di un pezzo segue il pezzo) si
    // sposta tutto dello stesso tratto `delta` (secondi, anche negativo). Senza spinta i pezzi e le clip degli altri non
    // si toccano (lo spostamento si ferma contro il primo); con la spinta, verso destra, quelli urtati scorrono davanti
    // (a catena) e i loro gruppi con loro. Le clip restano sulla loro traccia.
    // groupMoverCount: quanti pezzi e clip si muovono con `anchor` (1 = da solo).
    int groupMoverCount(ObjectRef anchor) const;
    // Il tratto più vicino a `delta` che si può fare senza sovrapporsi a nessuno (con la spinta verso destra non c'è limite).
    double clampGroupDelta(ObjectRef anchor, double delta, bool push) const;
    // Sposta il gruppo; false (e nessuna modifica) se `delta` non è possibile (vedi clampGroupDelta).
    bool moveGroup(ObjectRef anchor, double delta, bool push);
    // Spostare un pezzo video o una clip audio (`track` = la traccia di destinazione, solo per le clip). Se l'oggetto è
    // in un gruppo si muove tutto il gruppo (ognuno sulla sua traccia), altrimenti solo lui.
    // freeSpotFor: dove può andare partendo da `desired` (inizio nella sequenza), il punto più vicino che si può fare; -1
    // se l'oggetto non esiste.
    double freeSpotFor(ObjectRef object, int track, double desired, bool push) const;
    // I tratti di A1 occupati dall'audio dei pezzi video (quello non scollegato): le clip libere di A1 non ci stanno sopra.
    std::vector<TimeRange> linkedAudioRanges() const;
    struct MoveResult
    {
        bool moved = false;  // qualcosa si è spostato
        bool group = false;  // si è mosso un gruppo (più oggetti)
    };
    // Lo porta (o porta il gruppo) a `start`, entro quello che freeSpotFor permette.
    MoveResult moveObject(ObjectRef object, int track, double start, bool push);
    // Cambiare come sono fatti i gruppi senza toccare il montaggio (l'elenco dei gruppi): nessun oggetto si sposta o
    // si elimina. false (e nessuna modifica) se il gruppo o l'oggetto non ci sono, o non cambierebbe niente.
    // addToGroup: l'oggetto passa dal suo gruppo (se ne ha uno) a questo. removeFromGroup: l'oggetto resta libero; il
    // gruppo con meno di due oggetti si scioglie. renameGroup: nome vuoto = automatico.
    bool addToGroup(int groupId, ObjectRef object);
    bool removeFromGroup(ObjectRef object);
    bool renameGroup(int groupId, const std::string& name);
    // Come validGroups, con id e nome (per salvarli).
    std::vector<ObjectGroup> validGroupList() const;
    // I gruppi dell'utente senza gli oggetti che non esistono più, con almeno due oggetti (per salvarli).
    std::vector<std::vector<ObjectRef>> validGroups() const;

    // Giunzioni audio: due clip audio attaccate sulla stessa traccia, o due pezzi video attaccati (la loro audio su A1).
    // Su una giunzione c'è un pallino del livello comune: un punto sul bordo finale del primo e uno sul bordo iniziale
    // del secondo, allo stesso livello. Spostarlo cambia il volume di tutti e due; poi le linee dei due restano
    // indipendenti (altri punti si aggiungono e si spostano come sempre) e il pallino si toglie come gli altri.
    struct Junction
    {
        bool fragment = false;  // pezzi video (id dei pezzi); altrimenti clip audio (id delle clip)
        int first = 0;          // il pezzo o la clip che precede
        int second = 0;         // quello che segue

        bool operator==(const Junction&) const = default;
    };
    // Tutte le giunzioni (audio dei pezzi non scollegato) nell'ordine: prima i pezzi, poi le clip per traccia.
    std::vector<Junction> junctions() const;
    // Il livello (dB) del pallino comune; std::nullopt se non c'è (nessun punto sul bordo da una delle due parti, o con
    // livelli diversi) o la giunzione non esiste più.
    std::optional<double> junctionLevel(const Junction& junction) const;
    // Mette il pallino al livello `db` (lo crea o lo sposta). false se la giunzione non c'è o non cambia nulla.
    bool setJunctionLevel(const Junction& junction, double db);
    // Toglie i due punti sul bordo. false se non ce ne sono.
    bool removeJunctionPoint(const Junction& junction);
    // Dopo uno spostamento: dove due clip audio (non pezzi video: la linea di un pezzo costringe a mixare l'audio
    // esportando) sono ora attaccate e prima no (`before` = junctions() di prima), crea il pallino se in quel punto le
    // due linee hanno già lo stesso livello e non hanno punti sul bordo. Restituisce quanti ne ha creati.
    int linkNewJunctions(const std::vector<Junction>& before);

    // Formato del progetto (vedi ProjectFormat): vuoto finché non lo si decide, di solito al primo pezzo messo nel
    // montaggio. Fa parte delle modifiche (Annulla e Ripeti lo riportano com'era).
    const ProjectFormat& format() const { return format_; }
    void setFormat(ProjectFormat format) { format_ = std::move(format); }

    // Torna allo stato di `snapshot` (Annulla / Ripeti): file, sequenza e audio. L'analisi dei file già fatta qui
    // (fotogrammi chiave e durata) resta, anche se nell'istantanea il file non era ancora analizzato.
    void restoreEdits(const Project& snapshot);

private:
    std::vector<Clip> clips_;
    AudioTracks audio_;
    TextTracks text_;
    TransitionList transitions_;
    std::vector<NamedTextStyle> textStyles_;
    ProjectFormat format_;
    Sequence sequence_;
    Groups groups_;
    int nextId_ = 1;
};

}  // namespace editor::core
