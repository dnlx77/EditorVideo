#pragma once

#include <vector>

namespace editor::core {

// Segmento da tenere di un file, delimitato da IN e OUT (tempi in secondi dall'inizio del file).
//
// IN e OUT restano esattamente dove li mette l'utente (taglio preciso: all'esportazione la parte fra IN e il primo
// fotogramma chiave si ricodifica, vedi exportClips). Finché IN o OUT non sono impostati valgono l'inizio e la fine del
// file.
class CutSegment
{
public:
    // Nuovo file: azzera IN e OUT. `keyframes` in ordine crescente; vuoto = nessun aggancio (es. file solo audio).
    void reset(std::vector<double> keyframes, double duration);

    // Imposta IN (dentro il file) e restituisce il tempo effettivo. Se OUT non sta più dopo il nuovo IN, OUT viene tolto.
    double setIn(double seconds);
    // Imposta OUT; false (e nessuna modifica) se non sta dopo IN.
    bool setOut(double seconds);
    void clearIn();
    void clearOut();

    bool hasIn() const { return hasIn_; }
    bool hasOut() const { return hasOut_; }
    double in() const;   // inizio del segmento (0 se IN non è impostato)
    double out() const;  // fine del segmento (fine del file se OUT non è impostato)
    double length() const;

    double duration() const { return duration_; }
    const std::vector<double>& keyframes() const { return keyframes_; }

    // Fotogramma chiave nello stesso punto o subito prima di `seconds`; se `seconds` viene prima del primo
    // fotogramma chiave, il primo. Senza fotogrammi chiave restituisce `seconds`.
    double snapToKeyframe(double seconds) const;

private:
    double clampToFile(double seconds) const;

    std::vector<double> keyframes_;
    double duration_ = 0.0;
    bool hasIn_ = false;
    bool hasOut_ = false;
    double in_ = 0.0;
    double out_ = 0.0;
};

}  // namespace editor::core
