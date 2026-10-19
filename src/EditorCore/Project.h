#pragma once

#include "EditorCore/Sequence.h"

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
};

// Progetto: i file di partenza e la sequenza (il montaggio) fatta con pezzi di quei file.
class Project
{
public:
    // Aggiunge un file e ne restituisce l'id. L'analisi arriva dopo con setAnalysis().
    int add(std::string path);
    void setAnalysis(int id, std::vector<double> keyframes, double duration);
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
    // Aggancio ai fotogrammi chiave dei file del progetto, per le operazioni sulla sequenza.
    Sequence::SnapFunction snapper() const;

    // La sequenza come elenco di file e tratti da esportare (pezzi consecutivi dello stesso file insieme).
    std::vector<ClipRanges> exportPlan() const;

private:
    std::vector<Clip> clips_;
    Sequence sequence_;
    int nextId_ = 1;
};

}  // namespace editor::core
