#pragma once

#include "EditorCore/Groups.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <functional>
#include <vector>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace editor::app {

// Un oggetto in un gruppo, come lo mostra l'elenco.
struct GroupEntry
{
    editor::core::ObjectRef ref;
    QString kind;  // "Video", "Audio", "Audio del video", "Scritta"
    QString name;  // file o testo
    double start = 0.0;  // dove inizia nel montaggio, in secondi

    bool operator==(const GroupEntry&) const = default;
};

// Un gruppo, come lo mostra l'elenco: i gruppi fatti dall'utente (id > 0) e i video con il loro audio, legati da quando
// nascono (natural).
struct GroupRow
{
    int id = 0;
    bool natural = false;
    QString title;
    std::vector<GroupEntry> entries;

    bool operator==(const GroupRow&) const = default;
};

// L'elenco dei gruppi del montaggio: un albero con un nodo per gruppo e, dentro, i suoi oggetti. Il pannello non
// modifica mai il progetto: lo riempie la finestra con setGroups, e dice cosa si è scelto con onChosen.
class GroupListPanel : public QWidget
{
public:
    explicit GroupListPanel(QWidget* parent = nullptr);

    // Rimette l'elenco; se non è cambiato niente non tocca la vista (così restano scelta e nodi aperti o chiusi).
    void setGroups(std::vector<GroupRow> rows);
    const std::vector<GroupRow>& rows() const { return rows_; }
    int groupCount() const;
    QTreeWidget* tree() const { return tree_; }

    // Evidenzia nell'elenco ciò che è scelto sulla linea del tempo: se sono tutti gli oggetti di un gruppo, il suo nodo; se è
    // uno solo di un gruppo, quell'oggetto; altrimenti niente. Non chiama onChosen.
    void showSelection(const std::vector<editor::core::ObjectRef>& selected);

    // Si è scelto (con il clic o con la tastiera) un gruppo (tutti i suoi oggetti) o un oggetto: gli oggetti e il tempo
    // d'inizio più presto fra loro.
    std::function<void(const std::vector<editor::core::ObjectRef>&, double start)> onChosen;

    // Si trascinano oggetti dell'elenco (Ctrl+clic per più d'uno) su un gruppo o nello spazio vuoto. Il pannello non
    // cambia niente: dice cosa è stato trascinato e dove (groupId > 0: su quel gruppo; 0: nello spazio vuoto). Quello
    // che non cambierebbe niente (oggetti già in quel gruppo) non viene segnalato. Lasciare voci su un video con il suo
    // audio le unisce a loro in un gruppo nuovo (groupId 0, con anche il video e l'audio fra gli oggetti); le voci di un
    // video con il suo audio non si trascinano.
    std::function<void(const std::vector<editor::core::ObjectRef>&, int groupId)> onDropped;
    void dropEntries(const std::vector<editor::core::ObjectRef>& dragged, int groupId);
    // Gli oggetti scelti nell'elenco che si possono trascinare (voci dei gruppi dell'utente).
    std::vector<editor::core::ObjectRef> draggableSelection() const;

private:
    QTreeWidget* tree_ = nullptr;
    QLabel* empty_ = nullptr;
    std::vector<GroupRow> rows_;
    bool choosing_ = false;  // onChosen è in corso
    bool showing_ = false;  // showSelection sta cambiando la scelta: non è un clic dell'utente
    void choose(const QTreeWidgetItem* item);
    std::vector<int> collapsed_;  // id dei gruppi chiusi dall'utente (restano chiusi quando l'elenco si rifà)
};

}  // namespace editor::app
