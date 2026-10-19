#pragma once

#include "EditorCore/SegmentList.h"

#include <string>
#include <vector>

namespace editor::core {

// Un file del progetto con i suoi tratti da tenere.
struct Clip
{
    int id = 0;          // identificativo stabile (non cambia riordinando)
    std::string path;    // UTF-8
    bool analyzed = false;  // fotogrammi chiave e durata noti
    SegmentList segments;   // vuoto = tutto il file

    // Tratti che finiranno nel file esportato: quelli dell'elenco, oppure il file intero.
    std::vector<TimeRange> exportRanges() const;
    double outputLength() const;
};

// Tratti di un file da esportare, uno dopo l'altro.
struct ClipRanges
{
    std::string path;
    std::vector<TimeRange> ranges;
};

// Progetto: elenco ordinato di file da unire, ognuno con i suoi tratti.
class Project
{
public:
    // Aggiunge un file in fondo e ne restituisce l'id. L'analisi arriva dopo con setAnalysis().
    int add(std::string path);
    // Fotogrammi chiave e durata del file: azzera i suoi tratti.
    void setAnalysis(int id, std::vector<double> keyframes, double duration);
    void remove(int id);
    // Sposta il file in posizione `to` (0 = primo).
    void move(int id, int to);
    void clear();

    const std::vector<Clip>& clips() const { return clips_; }
    Clip* find(int id);
    const Clip* find(int id) const;
    int indexOf(int id) const;  // -1 se non c'è
    bool empty() const { return clips_.empty(); }
    bool allAnalyzed() const;

    // Tutti i tratti di tutti i file, nell'ordine in cui verranno uniti (i file senza tratti non ne hanno).
    std::vector<ClipRanges> exportPlan() const;
    double totalLength() const;

private:
    std::vector<Clip> clips_;
    int nextId_ = 1;
};

}  // namespace editor::core
