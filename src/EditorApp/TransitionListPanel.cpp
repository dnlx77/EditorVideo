#include "EditorApp/TransitionListPanel.h"

#include "EditorApp/TransitionMime.h"
#include "EditorApp/TransitionPreview.h"

#include <QtCore/QMimeData>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QVBoxLayout>

namespace editor::app {

namespace {

constexpr QSize kThumbnail(96, 54);

// L'elenco da cui si trascinano le transizioni: il trascinamento porta l'id della transizione (kTransitionMime).
class TransitionListWidget : public QListWidget
{
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* mime = new QMimeData;
        if (!items.isEmpty())
            mime->setData(QLatin1String(kTransitionMime), items.front()->data(Qt::UserRole).toString().toUtf8());
        return mime;
    }
};

}  // namespace

TransitionListPanel::TransitionListPanel(QWidget* parent) : QWidget(parent)
{
    list_ = new TransitionListWidget;
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setDragEnabled(true);
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setDefaultDropAction(Qt::CopyAction);
    list_->setIconSize(kThumbnail);
    list_->setToolTip(QStringLiteral("Le transizioni: clic per vederne l'anteprima nella Sorgente · trascinala sulla giunzione "
                                     "fra due pezzi nella linea del tempo per metterla nel montaggio"));
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
    // Anche un clic sulla voce già scelta (dopo aver guardato un file) rimette l'anteprima della transizione.
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
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
