#pragma once

#include "EditorCore/SegmentList.h"

#include <functional>
#include <vector>

namespace editor::core {

// Un pezzo di un file messo nella sequenza (il montaggio).
struct Fragment
{
    int id = 0;      // identificativo stabile (non cambia spostando i pezzi)
    int clipId = 0;  // file di origine (vedi Project)
    double start = 0.0;  // nel file di origine; sempre su un fotogramma chiave
    double end = 0.0;

    double length() const { return end - start; }
};

// Sequenza: i pezzi, nell'ordine in cui finiranno nel file esportato. Lo stesso file può comparire più volte,
// in qualsiasi ordine. Come altrove, senza ricodifica ogni pezzo deve iniziare su un fotogramma chiave: la funzione
// `snap` dice qual è il fotogramma chiave nello stesso punto o subito prima, per un file.
class Sequence
{
public:
    using SnapFunction = std::function<double(int clipId, double seconds)>;

    // Inserisce [start, end) del file in posizione `index` (-1 = in fondo); l'inizio viene agganciato al
    // fotogramma chiave. Restituisce l'id del pezzo, 0 se il pezzo è vuoto.
    int insert(int index, int clipId, double start, double end, const SnapFunction& snap);
    bool remove(int fragmentId);
    void removeClip(int clipId);  // tutti i pezzi di un file (quando il file esce dal progetto)
    void move(int fragmentId, int toIndex);
    void clear();

    // Divide il pezzo che si trova al tempo `seconds` della sequenza. La seconda parte deve iniziare su un
    // fotogramma chiave: si divide al fotogramma chiave nello stesso punto o subito prima (nessun fotogramma perso
    // o ripetuto). Restituisce l'id della seconda parte, 0 se lì non si può dividere.
    int split(double seconds, const SnapFunction& snap);

    // Toglie [from, to) del file `clipId` dai pezzi che lo contengono (come "taglia via" dello step 5).
    SegmentList::RemoveResult removeSourceRange(int clipId, double from, double to, const SnapFunction& snap);

    const std::vector<Fragment>& fragments() const { return fragments_; }
    bool empty() const { return fragments_.empty(); }
    const Fragment* find(int fragmentId) const;
    int indexOf(int fragmentId) const;  // -1 se non c'è
    double totalLength() const;
    double startOf(int index) const;    // dove inizia il pezzo `index` nella sequenza
    int indexAt(double seconds) const;  // pezzo che si trova a quel tempo della sequenza (-1 se oltre la fine)

private:
    std::vector<Fragment> fragments_;
    int nextId_ = 1;
};

}  // namespace editor::core
