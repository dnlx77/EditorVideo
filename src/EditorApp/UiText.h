#pragma once

// Testi comuni dell'interfaccia.

#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"

#include <QtCore/QString>

#include <string>

namespace editor::app {

// Tempo per l'utente, es. "00:01:02.500" (vedi core::formatDuration).
inline QString timeText(double seconds)
{
    return QString::fromStdString(core::formatDuration(seconds));
}

// Solo il nome del file di un percorso UTF-8.
inline QString fileNameOf(const std::string& utf8Path)
{
    return QString::fromStdString(core::fileNameOf(utf8Path));
}

}  // namespace editor::app
