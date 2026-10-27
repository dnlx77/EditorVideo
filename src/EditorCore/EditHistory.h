#pragma once

#include "EditorCore/Project.h"

#include <cstddef>
#include <string>
#include <vector>

namespace editor::core {

// Annulla / Ripeti per le modifiche al progetto (file, montaggio, audio), con istantanee: prima di ogni modifica si
// registra lo stato del progetto, e annullare vuol dire tornare a quello stato. Le istantanee riguardano solo ciò che
// l'utente ha cambiato: l'analisi dei file (fotogrammi chiave e durata) fatta nel frattempo resta (vedi
// Project::restoreEdits).
class EditHistory
{
public:
    explicit EditHistory(std::size_t limit = 200);

    // Prima di una modifica (o subito dopo, passando lo stato di prima): `before` è lo stato da ritrovare annullando.
    // Toglie la possibilità di ripetere.
    void record(std::string label, const Project& before);

    // Cambia ancora lo stato dopo l'ultima modifica registrata, senza una voce nuova (es. si continua a scrivere in un
    // pannello): annullando si torna comunque a prima della prima modifica. Lo stato salvato non si ritrova più.
    // false, e nessun effetto, se non c'è una voce da proseguire o c'è qualcosa da ripetere.
    bool amend();
    std::size_t undoCount() const { return undo_.size(); }  // per sapere se l'ultima voce è ancora la stessa

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const;  // es. "Dividi pezzo"; vuoto se non c'è niente
    std::string redoLabel() const;

    // Riportano `project` allo stato prima (o dopo) l'ultima modifica. false se non c'è niente da fare.
    bool undo(Project& project);
    bool redo(Project& project);

    void clear();  // es. aprendo un altro progetto; lo stato attuale diventa quello "salvato"
    // Lo stato attuale è quello salvato (dopo Salva): l'asterisco nel titolo segue isClean().
    void setClean();
    bool isClean() const;

private:
    struct Entry
    {
        std::string label;
        Project state;
    };
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::size_t limit_;
    // Quante modifiche ci sono fra l'inizio dell'elenco e lo stato salvato; -1 = non più raggiungibile.
    long long clean_ = 0;
};

}  // namespace editor::core
