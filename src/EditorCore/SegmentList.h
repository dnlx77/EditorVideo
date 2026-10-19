#pragma once

#include <vector>

namespace editor::core {

// Intervallo di tempo [start, end) in secondi dall'inizio del file.
struct TimeRange
{
    double start = 0.0;
    double end = 0.0;

    double length() const { return end - start; }
    bool operator==(const TimeRange&) const = default;
};

// Elenco dei tratti da tenere di un file, che verranno esportati uno dopo l'altro in un unico file.
//
// I tratti sono sempre in ordine di tempo e non si sovrappongono: quelli che si toccano o si sovrappongono
// vengono uniti. Come per IN (vedi CutSegment), ogni tratto inizia su un fotogramma chiave, quello nello stesso
// punto o subito prima: è l'unico modo di tagliare senza ricodifica. Per questo togliendo un pezzo, la parte
// tenuta dopo il pezzo tolto riparte dal fotogramma chiave più vicino prima della sua fine, e qualche fotogramma
// del pezzo tolto può restare.
class SegmentList
{
public:
    // Nuovo file: svuota l'elenco. `keyframes` vuoto = nessun aggancio (es. file solo audio).
    void reset(std::vector<double> keyframes, double duration);

    // Tiene [in, out). false (e nessuna modifica) se dopo l'aggancio di `in` il tratto è vuoto.
    bool add(double in, double out);
    // Esito di remove(): se non è Removed l'elenco non cambia.
    enum class RemoveResult
    {
        Removed,
        Empty,             // [in, out) vuoto o al contrario
        OutsideKept,       // [in, out) non tocca nessun tratto tenuto: è già escluso
        NoKeyframeInside,  // tocca un tratto, ma dentro non c'è un fotogramma chiave da cui far ripartire il resto
    };

    // Toglie [in, out) da quello che si tiene. Con l'elenco vuoto si parte dal file intero, così "togli la
    // pubblicità" funziona subito.
    RemoveResult remove(double in, double out);
    // Elimina il tratto che contiene `seconds`; false se lì non c'è nessun tratto.
    bool removeAt(double seconds);
    void clear();

    const std::vector<TimeRange>& ranges() const { return ranges_; }
    bool empty() const { return ranges_.empty(); }
    int indexAt(double seconds) const;  // -1 se `seconds` non è dentro un tratto
    double totalLength() const;         // durata del file che verrà esportato

    double duration() const { return duration_; }
    double snapToKeyframe(double seconds) const;

private:
    double clampToFile(double seconds) const;
    void normalize();

    std::vector<double> keyframes_;
    double duration_ = 0.0;
    std::vector<TimeRange> ranges_;
};

}  // namespace editor::core
