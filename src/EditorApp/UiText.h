#pragma once

#include <cmath>

// Testi comuni dell'interfaccia.

#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QString>

#include <string>

namespace editor::app {

// Tempo per l'utente, es. "00:01:02.500" (vedi core::formatDuration).
inline QString timeText(double seconds)
{
    return QString::fromStdString(core::formatDuration(seconds));
}

// Le impostazioni dell'utente. Senza il nome dell'organizzazione QSettings non scrive niente; `application` serve alle
// prove, che usano un nome diverso per non toccare le impostazioni vere.
inline QSettings settings(const QString& application = QStringLiteral("EditorVideo"))
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("EditorVideo"), application);
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

// Il segno di velocità accanto al nome di un pezzo o di una clip: « · 2x», « · ◄ 0.5x» (inverso), vuoto a velocità normale.
inline QString speedTag(double speed, bool reverse)
{
    const bool normal = std::abs(speed - 1.0) < 1e-9;
    if (normal && !reverse)
        return QString();
    QString text = QStringLiteral(" · ");
    if (reverse)
        text += QStringLiteral("◄ ");
    text += normal ? QStringLiteral("inverso") : QStringLiteral("%1x").arg(QString::number(speed, 'g', 3));
    return text;
}

}  // namespace editor::app
