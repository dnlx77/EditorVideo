#pragma once

// Testi comuni dell'interfaccia.

#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QString>

#include <string>

namespace editor::app {

// Tempo per l'utente, es. "00:01:02.500" (vedi core::formatDuration).
inline QString timeText(double seconds)
{
    return QString::fromStdString(core::formatDuration(seconds));
}

// Da testo UTF-8 a QString.
inline QString qs(const std::string& utf8)
{
    return QString::fromStdString(utf8);
}

// Percorso per il nucleo (FFmpeg, file di progetto): UTF-8 con le barre di Windows.
inline std::string nativeUtf8(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdString();
}

// Percorso assoluto con le barre di Windows (QString e UTF-8).
inline QString nativeAbsolute(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}
inline std::string nativeAbsoluteUtf8(const QString& path)
{
    return nativeAbsolute(path).toStdString();
}

// Solo il nome del file di un percorso UTF-8.
inline QString fileNameOf(const std::string& utf8Path)
{
    return QString::fromStdString(core::fileNameOf(utf8Path));
}

}  // namespace editor::app
