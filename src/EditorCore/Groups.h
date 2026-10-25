#pragma once

#include <vector>

namespace editor::core {

// Un oggetto del montaggio che si può mettere in un gruppo: un pezzo video, l'audio di un pezzo (su A1, finché non è
// scollegato) o una clip audio.
struct ObjectRef
{
    enum class Kind
    {
        Piece,       // id di un pezzo (Fragment)
        PieceAudio,  // id del pezzo a cui appartiene l'audio
        AudioClip,   // id di una clip audio (AudioClip)
    };
    Kind kind = Kind::Piece;
    int id = 0;

    bool operator==(const ObjectRef&) const = default;
    bool operator<(const ObjectRef& other) const
    {
        return kind != other.kind ? kind < other.kind : id < other.id;
    }
};

// Un gruppo fatto dall'utente: due o più oggetti che si muovono insieme. Un oggetto sta al più in un gruppo.
struct ObjectGroup
{
    int id = 0;
    std::vector<ObjectRef> members;

    bool operator==(const ObjectGroup&) const = default;
};

// I gruppi del progetto (quelli fatti dall'utente: il legame naturale fra un pezzo video e il suo audio non è qui, lo
// ricava Project::groupMembers).
class Groups
{
public:
    // Crea un gruppo con questi oggetti (senza doppioni). Gli oggetti che stavano in altri gruppi passano in questo; i
    // gruppi rimasti con meno di due oggetti si sciolgono. Restituisce l'id, 0 se gli oggetti sono meno di due.
    int create(const std::vector<ObjectRef>& objects);
    bool dissolve(int groupId);
    int groupOf(ObjectRef object) const;  // 0 se non è in nessun gruppo
    const ObjectGroup* find(int groupId) const;
    const std::vector<ObjectGroup>& all() const { return groups_; }
    bool empty() const { return groups_.empty(); }
    void clear() { groups_.clear(); }
    // Riprende i gruppi da un elenco (riaprendo un progetto): ricevono id nuovi; quelli con meno di due oggetti si scartano.
    void restore(const std::vector<std::vector<ObjectRef>>& groups);
    // Toglie dai gruppi gli oggetti per cui `keep` dà false; i gruppi con meno di due oggetti si sciolgono.
    template <typename Keep>
    void prune(Keep keep)
    {
        for (ObjectGroup& g : groups_)
            std::erase_if(g.members, [&](const ObjectRef& o) { return !keep(o); });
        std::erase_if(groups_, [](const ObjectGroup& g) { return g.members.size() < 2; });
    }
    // Torna allo stato di `snapshot` (Annulla / Ripeti); gli id nuovi non riprendono quelli usati nel frattempo.
    void restoreEdits(const Groups& snapshot);

    bool operator==(const Groups& other) const { return groups_ == other.groups_; }

private:
    std::vector<ObjectGroup> groups_;
    int nextId_ = 1;
};

}  // namespace editor::core
