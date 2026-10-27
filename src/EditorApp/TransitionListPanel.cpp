#include "EditorApp/TransitionListPanel.h"

#include "EditorApp/TransitionPreview.h"

#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QVBoxLayout>

namespace editor::app {

namespace {
constexpr QSize kThumbnail(96, 54);
}

TransitionListPanel::TransitionListPanel(QWidget* parent) : QWidget(parent)
{
    list_ = new QListWidget;
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setIconSize(kThumbnail);
    list_->setToolTip(QStringLiteral("Le transizioni: clic per vederne l'anteprima nella Sorgente"));
    list_->setMinimumSize(40, 24);
    for (const core::Transition* transition : core::transitionRegistry()) {
        auto* item = new QListWidgetItem(QString::fromUtf8(transition->name()));
        item->setData(Qt::UserRole, QString::fromUtf8(transition->id()));
        const QImage frame = renderTransitionSample(*transition, {}, 0.5, kThumbnail);
        item->setIcon(QIcon(QPixmap::fromImage(frame)));
        list_->addItem(item);
    }
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem*) {
        if (onSelected)
            onSelected(selected());
    });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(list_);
}

int TransitionListPanel::count() const
{
    return list_->count();
}

const core::Transition* TransitionListPanel::selected() const
{
    const QListWidgetItem* item = list_->currentItem();
    return item == nullptr ? nullptr : core::findTransition(item->data(Qt::UserRole).toString().toStdString());
}

bool TransitionListPanel::select(const std::string& id)
{
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString().toStdString() == id) {
            list_->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

}  // namespace editor::app
