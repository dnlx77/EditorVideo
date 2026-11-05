#pragma once

#include "EditorCore/Animation.h"

#include <QtGui/QColor>

namespace editor::app {

// Il colore di ogni gruppo di parametri animabili: lo stesso sui rombi della linea del tempo e sul rombo del pannello Proprietà,
// così si capisce a quale trasformazione appartiene una chiave.
inline QColor animGroupColor(core::AnimGroup group)
{
    switch (group) {
        case core::AnimGroup::Scale: return QColor(70, 160, 255);     // azzurro
        case core::AnimGroup::Position: return QColor(70, 200, 100);  // verde
        case core::AnimGroup::Rotation: return QColor(255, 150, 40);  // arancione
        case core::AnimGroup::Crop: return QColor(190, 100, 235);     // viola
        case core::AnimGroup::Opacity: return QColor(235, 235, 235);  // bianco
    }
    return QColor(200, 200, 200);
}

// Il nome del gruppo come lo vede l'utente.
inline const char* animGroupName(core::AnimGroup group)
{
    switch (group) {
        case core::AnimGroup::Scale: return "Scala";
        case core::AnimGroup::Position: return "Posizione";
        case core::AnimGroup::Rotation: return "Rotazione";
        case core::AnimGroup::Crop: return "Ritaglio";
        case core::AnimGroup::Opacity: return "Opacità";
    }
    return "";
}

}  // namespace editor::app
