#pragma once

#include "EditorCore/Groups.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>

#include <vector>

namespace editor::app {

// Il tipo di dati che la linea del tempo mette nel trascinamento di oggetti (Alt+trascina) verso l'elenco dei gruppi:
// un oggetto per riga, "tipo id" (tipo = core::ObjectRef::Kind come numero).
inline constexpr const char* kGroupObjectsMime = "application/x-editorvideo-group-objects";

inline QByteArray encodeGroupObjects(const std::vector<editor::core::ObjectRef>& objects)
{
    QByteArray out;
    for (const editor::core::ObjectRef& o : objects)
        out += QByteArray::number(static_cast<int>(o.kind)) + ' ' + QByteArray::number(o.id) + '\n';
    return out;
}

inline std::vector<editor::core::ObjectRef> decodeGroupObjects(const QByteArray& data)
{
    std::vector<editor::core::ObjectRef> objects;
    for (const QByteArray& line : data.split('\n')) {
        const QList<QByteArray> parts = line.split(' ');
        bool okKind = false;
        bool okId = false;
        const int kind = parts.size() == 2 ? parts[0].toInt(&okKind) : 0;
        const int id = parts.size() == 2 ? parts[1].toInt(&okId) : 0;
        if (okKind && okId && kind >= 0 && kind <= static_cast<int>(editor::core::ObjectRef::Kind::TextClip))
            objects.push_back({static_cast<editor::core::ObjectRef::Kind>(kind), id});
    }
    return objects;
}

}  // namespace editor::app
