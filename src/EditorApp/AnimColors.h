#pragma once

#include "EditorCore/Animation.h"

#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QPolygonF>

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

// La sotto-riga di un gruppo animato: una fascia leggera nel suo colore e la linea che unisce le sue chiavi (`line`, i
// centri delle chiavi in ordine). La stessa nella linea del tempo e nella mini linea del pannello Proprietà.
inline void drawKeyBand(QPainter& p, const QRectF& band, const QColor& color, const QPolygonF& line)
{
    QColor fill = color;
    fill.setAlpha(34);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRect(band);
    if (line.size() >= 2) {
        QColor stroke = color;
        stroke.setAlpha(190);
        p.setPen(QPen(stroke, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);
    }
}

// Una chiave: la forma dice il tipo di passaggio (quadrato = fermo, tondo = dolce, rombo = lineare, rombo con un puntino
// scuro = curva); `half` è metà della diagonale del rombo.
inline void drawKeyMark(QPainter& p, const QPointF& center, double half, core::Interp interp, const QColor& color, const QPen& outline)
{
    p.setBrush(color);
    p.setPen(outline);
    const double x = center.x();
    const double y = center.y();
    switch (interp) {
        case core::Interp::Hold: p.drawRect(QRectF(x - half * 0.8, y - half * 0.8, half * 1.6, half * 1.6)); break;
        case core::Interp::Smooth: p.drawEllipse(center, half * 0.9, half * 0.9); break;
        case core::Interp::Linear:
        case core::Interp::Bezier: {
            QPolygonF diamond;
            diamond << QPointF(x, y - half) << QPointF(x + half, y) << QPointF(x, y + half) << QPointF(x - half, y);
            p.drawPolygon(diamond);
            if (interp == core::Interp::Bezier) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(20, 20, 20));
                p.drawEllipse(center, half * 0.28, half * 0.28);
            }
            break;
        }
    }
}

}  // namespace editor::app
