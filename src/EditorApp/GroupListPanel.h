#pragma once

#include "EditorCore/Groups.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <vector>

class QLabel;
class QTreeWidget;

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

// L'elenco dei gruppi del montaggio: un albero con un nodo per gruppo e, dentro, i suoi oggetti. Per ora si guarda
// soltanto: il pannello non modifica mai il progetto, lo riempie la finestra con setGroups.
class GroupListPanel : public QWidget
{
public:
    explicit GroupListPanel(QWidget* parent = nullptr);

    // Rimette l'elenco; se non è cambiato niente non tocca la vista (così restano scelta e nodi aperti o chiusi).
    void setGroups(std::vector<GroupRow> rows);
    const std::vector<GroupRow>& rows() const { return rows_; }
    int groupCount() const;
    QTreeWidget* tree() const { return tree_; }

private:
    QTreeWidget* tree_ = nullptr;
    QLabel* empty_ = nullptr;
    std::vector<GroupRow> rows_;
    std::vector<int> collapsed_;  // id dei gruppi chiusi dall'utente (restano chiusi quando l'elenco si rifà)
};

}  // namespace editor::app
