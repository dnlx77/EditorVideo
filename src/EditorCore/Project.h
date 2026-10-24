#pragma once

#include "EditorCore/AudioTracks.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/MixSpec.h"
#include "EditorCore/Sequence.h"

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

    // Traccia audio sostitutiva (vedi AudioReplacement), il caso più semplice delle tracce audio: impostarla toglie
    // tutte le tracce aggiunte e mette il file su A2 con A1 silenziata; std::nullopt rimette le tracce come
    // all'inizio (solo A1). audioReplacement() la ricava dalle tracce: c'è solo se sono proprio in quella forma
    // (A1 silenziata e una sola clip, su A2, fino alla fine del file, senza livelli cambiati).
    void setAudioReplacement(std::optional<AudioReplacement> replacement);
    std::optional<AudioReplacement> audioReplacement() const;
    void setAudioOffset(double offset);  // sposta la traccia sostitutiva
    // Tratti silenziati e traccia sostitutiva, per l'esportazione: nei tempi del file esportato (con gli spazi vuoti
    // trattati come dice `gaps`).
    AudioEdit audioEdit(GapExport gaps = GapExport::Close) const;
    // Se esportando l'audio non basta copiarlo com'è (vedi AudioTracks::needsMix) o i pezzi video hanno una linea del
    // livello: allora audioEdit() ha il mix.
    bool audioNeedsMix() const;

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
    ProjectFormat format_;
    Sequence sequence_;
    int nextId_ = 1;
};

}  // namespace editor::core
