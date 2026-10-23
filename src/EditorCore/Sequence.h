#pragma once

#include "EditorCore/TimeUtil.h"

#include <vector>

namespace editor::core {

// Un pezzo di un file messo nella sequenza (il montaggio).
struct Fragment
{
    int id = 0;      // identificativo stabile (non cambia spostando i pezzi)
    int clipId = 0;  // file di origine (vedi Project)
    double start = 0.0;  // nel file di origine (anche fra due fotogrammi chiave: all'esportazione si ricodifica)
    double end = 0.0;
    // Tratti in cui l'audio è tolto (silenzio), nei tempi del file di origine: in ordine, senza sovrapposizioni,
    // dentro [start, end). Stanno nel pezzo, così lo seguono se viene spostato.
    std::vector<TimeRange> muted;
    // Spazio vuoto nella sequenza subito prima del pezzo, in secondi (0 = attaccato al precedente o all'inizio).
    double gap = 0.0;

    double length() const { return end - start; }
};

// Sequenza: i pezzi, nell'ordine in cui finiranno nel file esportato. Lo stesso file può comparire più volte,
// in qualsiasi ordine. I pezzi iniziano e finiscono dove li si mette (taglio preciso: all'esportazione si ricodifica
// la parte fino al fotogramma chiave dopo).
//
// Fra un pezzo e l'altro (e prima del primo) ci può essere uno spazio vuoto (Fragment::gap): nell'anteprima è nero e
// silenzio. Dopo l'ultimo pezzo non c'è spazio: la sequenza finisce lì.
class Sequence
{
public:
    // Inserisce [start, end) del file in posizione `index` (-1 = in fondo). Restituisce l'id del pezzo, 0 se il pezzo
    // è vuoto.
    // Il pezzo va subito dopo il precedente: se lì c'era uno spazio vuoto lo riempie partendo dal suo inizio, senza
    // spostare gli altri pezzi; se non ci sta, quelli dopo vanno avanti quanto serve.
    int insert(int index, int clipId, double start, double end);
    // Toglie il pezzo; quelli dopo scorrono indietro della sua durata (lo spazio vuoto prima di lui resta).
    bool remove(int fragmentId);
    void removeClip(int clipId);  // tutti i pezzi di un file (quando il file esce dal progetto)
    void move(int fragmentId, int toIndex);  // cambia l'ordine (gli spazi vuoti restano dove sono)
    void clear();
    // Rimette i pezzi così come sono (es. riaprendo un progetto salvato): ognuno riceve un id nuovo.
    void restore(std::vector<Fragment> fragments);

    // Spazi vuoti. Un pezzo si sposta da solo, senza muovere gli altri, e non si può sovrapporre a nessuno.
    // Dove può iniziare il pezzo se lo si vuole far iniziare a `desired` (tempo della sequenza): il punto libero più
    // vicino, cioè `desired` se lì ci sta, altrimenti contro il pezzo che lo blocca (o in un altro spazio, se è più
    // vicino). -1 se il pezzo non c'è.
    double nearestFreeStart(int fragmentId, double desired) const;
    // Sposta il pezzo in modo che inizi a `start`; false (e nessuna modifica) se si sovrapporrebbe ad altri.
    bool moveTo(int fragmentId, double start);
    // Pezzo che ha subito prima lo spazio vuoto in cui si trova `seconds` (-1 se lì non c'è uno spazio vuoto).
    int gapIndexAt(double seconds) const;
    // Chiude lo spazio vuoto prima del pezzo `index`: quel pezzo e i successivi scorrono indietro.
    bool closeGap(int index);
    void closeAllGaps();
    int gapCount() const;

    // Divide il pezzo che si trova al tempo `seconds` della sequenza, proprio lì. Restituisce l'id della seconda
    // parte, 0 se lì non si può dividere (fuori dai pezzi, in uno spazio vuoto o sul bordo di un pezzo).
    int split(double seconds);

    // Esito di removeSourceRange(): se non è Removed la sequenza non cambia.
    enum class RemoveResult
    {
        Removed,
        Empty,             // [from, to) vuoto o al contrario
        OutsideKept,       // [from, to) non tocca nessun pezzo del file: è già escluso
    };

    // Toglie [from, to) del file `clipId` dai pezzi che lo contengono; la parte dopo riparte da `to`.
    RemoveResult removeSourceRange(int clipId, double from, double to);

    // Audio: toglie (silenzio) o rimette l'audio in [from, to) della sequenza, nei pezzi che lo attraversano.
    // Restituiscono true se qualcosa è cambiato.
    bool muteRange(double from, double to);
    bool unmuteRange(double from, double to);
    // Tutti i tratti senza audio, nei tempi della sequenza, in ordine e uniti se si toccano.
    std::vector<TimeRange> mutedRanges() const;

    const std::vector<Fragment>& fragments() const { return fragments_; }
    bool empty() const { return fragments_.empty(); }
    const Fragment* find(int fragmentId) const;
    int indexOf(int fragmentId) const;  // -1 se non c'è
    double totalLength() const;         // fino alla fine dell'ultimo pezzo, spazi vuoti compresi
    double startOf(int index) const;    // dove inizia il pezzo `index` nella sequenza
    std::vector<double> starts() const;  // l'inizio di ogni pezzo, in una volta sola (startOf per tutti)
    // Pezzo che si trova a quel tempo della sequenza (-1 se oltre la fine o in uno spazio vuoto).
    int indexAt(double seconds) const;

private:
    // Modifica di un elenco di tratti di un pezzo (nei tempi del file): aggiunge o toglie [from, to).
    using RangeOp = void (*)(std::vector<TimeRange>& ranges, double from, double to);
    // Applica `op` ai tratti senza audio dei pezzi attraversati da [from, to) della sequenza; true se è cambiato qualcosa.
    bool applyToRange(double from, double to, RangeOp op);

    std::vector<Fragment> fragments_;
    int nextId_ = 1;
};

}  // namespace editor::core
