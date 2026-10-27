#pragma once

#include "EditorCore/Transitions.h"

#include <QtWidgets/QWidget>

#include <functional>
#include <string>

class QListWidget;

namespace editor::app {

// L'elenco delle transizioni disponibili (tutte quelle del registro, nel suo ordine), con nome e una miniatura a metà
// effetto. Si sceglie una voce con un clic.
class TransitionListPanel : public QWidget
{
public:
    explicit TransitionListPanel(QWidget* parent = nullptr);

    int count() const;
    // La transizione scelta (nullptr = nessuna).
    const core::Transition* selected() const;
    // Sceglie la transizione con quell'id; false se non c'è.
    bool select(const std::string& id);
    QListWidget* list() const { return list_; }

    // Cambia la scelta (nullptr = non c'è più niente di scelto). Il progetto non usa moc: niente segnali propri.
    std::function<void(const core::Transition*)> onSelected;

private:
    QListWidget* list_ = nullptr;
};

}  // namespace editor::app
