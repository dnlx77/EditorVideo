#include "EditorApp/GroupListPanel.h"

#include "EditorApp/GroupMime.h"
#include "EditorApp/UiText.h"

#include <QtCore/QMimeData>
#include <QtCore/QTimer>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QFont>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace editor::app {

namespace {

// L'albero dei gruppi: accetta il trascinamento di voci sue su un gruppo o nello spazio vuoto, ma non sposta niente da
// solo (lo fa chi riceve `dropped`, cambiando il progetto e rifacendo l'elenco).
class GroupTree : public QTreeWidget
{
public:
    // target: il nodo del gruppo, nullptr = spazio vuoto; external: gli oggetti vengono dalla linea del tempo.
    std::function<bool(const QTreeWidgetItem* target, bool external)> canDrop;
    std::function<void(const QTreeWidgetItem* target, std::vector<editor::core::ObjectRef> external)> dropped;

protected:
    const QTreeWidgetItem* targetAt(const QPoint& at) const
    {
        const QTreeWidgetItem* item = itemAt(at);
        return item != nullptr && item->parent() != nullptr ? item->parent() : item;
    }
    static bool fromTimeline(const QMimeData* mime)
    {
        return mime->hasFormat(QLatin1String(kGroupObjectsMime));
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (fromTimeline(event->mimeData()))
            event->acceptProposedAction();
        else
            QTreeWidget::dragEnterEvent(event);
    }
    void dragMoveEvent(QDragMoveEvent* event) override
    {
        const bool external = event->source() != this && fromTimeline(event->mimeData());
        if (!external)
            QTreeWidget::dragMoveEvent(event);
        if ((external || event->source() == this) && canDrop && canDrop(targetAt(event->position().toPoint()), external)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
    }
    void dropEvent(QDropEvent* event) override
    {
        const bool external = event->source() != this && fromTimeline(event->mimeData());
        const QTreeWidgetItem* target = targetAt(event->position().toPoint());
        const std::vector<editor::core::ObjectRef> outside =
            external ? decodeGroupObjects(event->mimeData()->data(QLatin1String(kGroupObjectsMime))) : std::vector<editor::core::ObjectRef>{};
        if ((external || event->source() == this) && (!external || !outside.empty()) && dropped && canDrop && canDrop(target, external)) {
            dropped(target, outside);
            event->setDropAction(Qt::CopyAction);  // niente "sposta": le voci non si tolgono da sole
            event->accept();
        } else {
            event->ignore();
        }
        stopAutoScroll();
        viewport()->update();
    }
};

}  // namespace

GroupListPanel::GroupListPanel(QWidget* parent) : QWidget(parent)
{
    auto* tree = new GroupTree;
    tree_ = tree;
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDragDropMode(QAbstractItemView::DragDrop);
    tree_->setDefaultDropAction(Qt::CopyAction);
    tree->canDrop = [this](const QTreeWidgetItem* target, bool external) {
        if (!external && draggableSelection().empty())
            return false;
        if (target == nullptr)
            return true;
        const int row = tree_->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(target));
        return row >= 0 && row < static_cast<int>(rows_.size());
    };
    // Il cambiamento si fa dopo la fine del trascinamento: l'elenco si rifà e le voci trascinate spariscono.
    tree->dropped = [this](const QTreeWidgetItem* target, std::vector<editor::core::ObjectRef> external) {
        std::vector<editor::core::ObjectRef> dragged = external.empty() ? draggableSelection() : std::move(external);
        const int row = target == nullptr ? -1 : tree_->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(target));
        int groupId = row >= 0 && row < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(row)].id : 0;
        // Su un video con il suo audio: i due, già legati, si uniscono agli oggetti trascinati in un gruppo nuovo.
        if (row >= 0 && row < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(row)].natural) {
            for (const GroupEntry& e : rows_[static_cast<std::size_t>(row)].entries)
                dragged.push_back(e.ref);
            groupId = 0;
        }
        QTimer::singleShot(0, this, [this, dragged, groupId] { dropEntries(dragged, groupId); });
    };
    tree_->setRootIsDecorated(true);
    tree_->setMinimumSize(40, 24);
    tree_->setToolTip(QStringLiteral("I gruppi del montaggio: ognuno con gli oggetti che si muovono insieme. Trascina le voci per cambiare i gruppi, "
                                     "o Alt+trascina un oggetto dalla linea del tempo su un gruppo."));
    empty_ = new QLabel(QStringLiteral("Nessun gruppo nel montaggio.\nCon Ctrl+clic scegli più oggetti e premi G."));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);
    layout->addWidget(empty_);
    empty_->setVisible(true);
    tree_->setVisible(false);

    // Il nodo chiuso o aperto dall'utente si ricorda per id (i video con il loro audio non hanno id: restano aperti).
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) { choose(item); });
    // Anche un clic sulla voce già scelta (dopo aver scelto altro sulla linea del tempo) rifà la scelta.
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) { choose(item); });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        const int id = item->data(0, Qt::UserRole).toInt();
        if (id > 0 && std::find(collapsed_.begin(), collapsed_.end(), id) == collapsed_.end())
            collapsed_.push_back(id);
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        std::erase(collapsed_, item->data(0, Qt::UserRole).toInt());
    });
}

void GroupListPanel::setGroups(std::vector<GroupRow> rows)
{
    if (rows == rows_)
        return;
    rows_ = std::move(rows);
    tree_->clear();
    for (const GroupRow& row : rows_) {
        auto* top = new QTreeWidgetItem(tree_);
        // Tutti i nodi accolgono voci; quelle dei video con il loro audio, però, non si trascinano.
        top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
        top->setText(0, QStringLiteral("%1  (%2)").arg(row.title).arg(row.entries.size()));
        top->setData(0, Qt::UserRole, row.id);
        if (row.natural) {
            QFont font = top->font(0);
            font.setItalic(true);
            top->setFont(0, font);
            top->setToolTip(0, QStringLiteral("Un video con il suo audio: legati da quando il pezzo è nato. "
                                              "Con U sul pezzo l'audio diventa una clip libera su A1."));
        }
        for (const GroupEntry& entry : row.entries) {
            auto* child = new QTreeWidgetItem(top);
            child->setText(0, QStringLiteral("%1 · %2 · %3").arg(entry.kind, entry.name, timeText(entry.start)));
            child->setData(0, Qt::UserRole, -1);
            child->setFlags(row.natural ? Qt::ItemIsEnabled | Qt::ItemIsSelectable
                                        : Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        top->setExpanded(row.id <= 0 || std::find(collapsed_.begin(), collapsed_.end(), row.id) == collapsed_.end());
    }
    tree_->setVisible(!rows_.empty());
    empty_->setVisible(rows_.empty());
}

void GroupListPanel::choose(const QTreeWidgetItem* item)
{
    if (showing_ || item == nullptr || !onChosen)
        return;
    const QTreeWidgetItem* top = item->parent() != nullptr ? item->parent() : item;
    const int row = tree_->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(top));
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    const std::vector<GroupEntry>& entries = rows_[static_cast<std::size_t>(row)].entries;
    std::vector<editor::core::ObjectRef> refs;
    double start = 0.0;
    const auto add = [&](const GroupEntry& e) {
        start = refs.empty() ? e.start : std::min(start, e.start);
        refs.push_back(e.ref);
    };
    if (item == top) {
        for (const GroupEntry& e : entries)
            add(e);
    } else {
        const int index = top->indexOfChild(const_cast<QTreeWidgetItem*>(item));
        if (index >= 0 && index < static_cast<int>(entries.size()))
            add(entries[static_cast<std::size_t>(index)]);
    }
    if (refs.empty())
        return;
    choosing_ = true;  // la linea del tempo risponde con la sua selezione: l'elenco non si sposta da solo
    onChosen(refs, start);
    choosing_ = false;
}

void GroupListPanel::showSelection(const std::vector<editor::core::ObjectRef>& selected)
{
    if (choosing_)
        return;
    QTreeWidgetItem* target = nullptr;
    for (std::size_t r = 0; r < rows_.size() && target == nullptr && !selected.empty(); ++r) {
        const std::vector<GroupEntry>& entries = rows_[r].entries;
        const auto inRow = [&](const editor::core::ObjectRef& o) {
            return std::any_of(entries.begin(), entries.end(), [&](const GroupEntry& e) { return e.ref == o; });
        };
        if (!std::all_of(selected.begin(), selected.end(), inRow))
            continue;
        QTreeWidgetItem* top = tree_->topLevelItem(static_cast<int>(r));
        target = top;
        if (selected.size() == 1) {
            for (std::size_t e = 0; e < entries.size(); ++e) {
                if (entries[e].ref == selected.front())
                    target = top->child(static_cast<int>(e));
            }
        }
    }
    showing_ = true;
    tree_->setCurrentItem(target);
    if (target == nullptr)
        tree_->clearSelection();
    showing_ = false;
}

std::vector<editor::core::ObjectRef> GroupListPanel::draggableSelection() const
{
    std::vector<editor::core::ObjectRef> refs;
    for (const QTreeWidgetItem* item : tree_->selectedItems()) {
        const QTreeWidgetItem* top = item->parent();
        if (top == nullptr)
            continue;
        const int row = tree_->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(top));
        const int index = top->indexOfChild(const_cast<QTreeWidgetItem*>(item));
        if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].natural || index < 0
            || index >= static_cast<int>(rows_[static_cast<std::size_t>(row)].entries.size()))
            continue;
        refs.push_back(rows_[static_cast<std::size_t>(row)].entries[static_cast<std::size_t>(index)].ref);
    }
    return refs;
}

void GroupListPanel::dropEntries(const std::vector<editor::core::ObjectRef>& dragged, int groupId)
{
    if (dragged.empty() || !onDropped)
        return;
    // Quello che non cambierebbe niente non si segnala: tutti già nel gruppo di arrivo, o (nello spazio vuoto) tutto un
    // gruppo che si rifarebbe uguale.
    const auto rowOf = [&](const editor::core::ObjectRef& ref) {
        for (const GroupRow& row : rows_) {
            if (std::any_of(row.entries.begin(), row.entries.end(), [&](const GroupEntry& e) { return e.ref == ref; }))
                return &row;
        }
        return static_cast<const GroupRow*>(nullptr);
    };
    if (groupId > 0 && std::all_of(dragged.begin(), dragged.end(), [&](const editor::core::ObjectRef& r) {
            const GroupRow* row = rowOf(r);
            return row != nullptr && row->id == groupId;
        }))
        return;
    if (groupId == 0 && dragged.size() >= 2) {
        const GroupRow* first = rowOf(dragged.front());
        if (first != nullptr && first->entries.size() == dragged.size()
            && std::all_of(dragged.begin(), dragged.end(), [&](const editor::core::ObjectRef& r) { return rowOf(r) == first; }))
            return;
    }
    onDropped(dragged, groupId);
}

int GroupListPanel::groupCount() const
{
    return tree_->topLevelItemCount();
}

}  // namespace editor::app
