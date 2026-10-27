#pragma once

#include "EditorCore/Transitions.h"

#include <string>
#include <vector>

namespace editor::core {

// Una transizione messa nel montaggio: sta sulla giunzione fra due pezzi video attaccati (`beforeFragment` e subito dopo
// `afterFragment`). Il progetto (Project) controlla che la giunzione ci sia e che la durata ci stia.
struct TransitionClip
{
    int id = 0;               // identificativo stabile
    std::string type;         // id della transizione nel registro (vedi Transition::id); sconosciuto = non si disegna
    int beforeFragment = 0;   // il pezzo che finisce nella giunzione
    int afterFragment = 0;    // il pezzo che inizia nella giunzione
    double duration = 1.0;    // secondi, per intero
    TransitionParams params;
    bool crossfadeAudio = true;  // dissolvenza incrociata anche dell'audio dei due pezzi
};

// L'elenco delle transizioni del progetto, in ordine di inserimento. Solo l'archivio: i controlli sulla sequenza stanno
// in Project.
class TransitionList
{
public:
    // Aggiunge la transizione (l'id che porta viene ignorato: ne riceve uno nuovo) e restituisce l'id.
    int add(TransitionClip clip);
    bool remove(int id);
    void clear();
    // Rimette le transizioni così come sono (es. riaprendo un progetto salvato): ognuna riceve un id nuovo.
    void restore(std::vector<TransitionClip> clips);
    // Torna allo stato di `snapshot` (Annulla / Ripeti).
    void restoreEdits(const TransitionList& snapshot);

    const std::vector<TransitionClip>& clips() const { return clips_; }
    bool empty() const { return clips_.empty(); }
    TransitionClip* find(int id);
    const TransitionClip* find(int id) const;
    // La transizione che ha `beforeFragment` come pezzo prima della giunzione (nullptr se non c'è).
    const TransitionClip* atJunction(int beforeFragment) const;
    // Le transizioni dei pezzi `from` rimettono i loro pezzi a `to` (quando un pezzo diviso passa la fine al secondo
    // pezzo): vale per il pezzo prima della giunzione.
    void moveJunction(int fromBeforeFragment, int toBeforeFragment);

private:
    std::vector<TransitionClip> clips_;
    int nextId_ = 1;
};

}  // namespace editor::core
