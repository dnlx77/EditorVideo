#include "EditorApp/GroupListPanel.h"

#include "EditorApp/UiText.h"

#include <QtGui/QFont>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace editor::app {

GroupListPanel::GroupListPanel(QWidget* parent) : QWidget(parent)
{
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setRootIsDecorated(true);
    tree_->setMinimumSize(40, 24);
    tree_->setToolTip(QStringLiteral("I gruppi del montaggio: ognuno con gli oggetti che si muovono insieme"));
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

int GroupListPanel::groupCount() const
{
    return tree_->topLevelItemCount();
}

}  // namespace editor::app
