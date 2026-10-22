#pragma once

#include "EditorCore/Compatibility.h"
#include "EditorCore/Sequence.h"

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
    std::vector<double> keyframes;
    double duration = 0.0;

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
};

// Un file audio (MP3, AAC, M4A, WAV...) che prende il posto dell'audio originale di tutta la sequenza.
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

    bool empty() const { return muted.empty() && !replacement; }
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
    // Per le operazioni sulla sequenza: con il taglio preciso i tempi restano esattamente quelli scelti (le parti fra
    // un fotogramma chiave e un taglio si ricodificano all'esportazione), quindi nessun aggancio.
    Sequence::SnapFunction snapper() const;

    // La sequenza come elenco di file e tratti da esportare (pezzi consecutivi dello stesso file insieme), con lo spazio
    // vuoto prima di ogni tratto (ClipRanges::gapsBefore; con GapExport::Close nessuno).
    std::vector<ClipRanges> exportPlan(GapExport gaps = GapExport::Close) const;

    // Traccia audio sostitutiva (vedi AudioReplacement); std::nullopt = audio originale.
    void setAudioReplacement(std::optional<AudioReplacement> replacement);
    const std::optional<AudioReplacement>& audioReplacement() const { return replacement_; }
    void setAudioOffset(double offset);  // sposta la traccia sostitutiva
    // Tratti silenziati e traccia sostitutiva, per l'esportazione: nei tempi del file esportato (con gli spazi vuoti
    // trattati come dice `gaps`).
    AudioEdit audioEdit(GapExport gaps = GapExport::Close) const;

    // Formato del progetto (vedi ProjectFormat): vuoto finché non lo si decide, di solito al primo pezzo messo nel
    // montaggio. Fa parte delle modifiche (Annulla e Ripeti lo riportano com'era).
    const ProjectFormat& format() const { return format_; }
    void setFormat(ProjectFormat format) { format_ = std::move(format); }

    // Torna allo stato di `snapshot` (Annulla / Ripeti): file, sequenza e audio. L'analisi dei file già fatta qui
    // (fotogrammi chiave e durata) resta, anche se nell'istantanea il file non era ancora analizzato.
    void restoreEdits(const Project& snapshot);

private:
    std::vector<Clip> clips_;
    std::optional<AudioReplacement> replacement_;
    ProjectFormat format_;
    Sequence sequence_;
    int nextId_ = 1;
};

}  // namespace editor::core
