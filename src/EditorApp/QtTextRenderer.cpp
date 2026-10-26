#include "EditorApp/QtTextRenderer.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace editor::app {

namespace {

QColor toQt(const core::TextColor& c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

struct Line
{
    QString text;
    double width = 0.0;
};

}  // namespace

core::TextImage QtTextRenderer::render(const core::TextClip& clip, int frameWidth, int frameHeight) const
{
    const core::TextStyle& style = clip.style;
    core::TextImage result;
    if (frameWidth <= 0 || frameHeight <= 0 || clip.text.empty())
        return result;

    const double pixelSize = std::max(1.0, style.size * frameHeight);
    QFont font(QString::fromUtf8(style.fontFamily.c_str()));
    font.setPixelSize(static_cast<int>(std::lround(pixelSize)));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    const QFontMetricsF metrics(font);

    // Righe e misure del testo.
    const QStringList texts = QString::fromUtf8(clip.text.c_str()).split(QLatin1Char('\n'));
    std::vector<Line> lines;
    double textWidth = 0.0;
    for (const QString& t : texts) {
        Line line{t, metrics.horizontalAdvance(t)};
        textWidth = std::max(textWidth, line.width);
        lines.push_back(std::move(line));
    }
    const double lineHeight = metrics.lineSpacing() * style.lineSpacing;
    const double textHeight = lineHeight * static_cast<double>(lines.size());

    // Il riquadro: testo, margine dello sfondo e quanto sporgono contorno e ombra.
    const double padding = style.background ? style.backgroundPadding * frameHeight : 0.0;
    const double outline = style.outline ? style.outlineWidth * frameHeight : 0.0;
    const double shadowX = style.shadow ? style.shadowOffsetX * frameHeight : 0.0;
    const double shadowY = style.shadow ? style.shadowOffsetY * frameHeight : 0.0;
    const double reach = outline;  // il contorno sporge dalle lettere di quanto è largo
    const double boxWidth = textWidth + 2.0 * padding;
    const double boxHeight = textHeight + 2.0 * padding;
    const double left = std::min({0.0, shadowX}) - reach;
    const double top = std::min({0.0, shadowY}) - reach;
    const double right = boxWidth + std::max({0.0, shadowX}) + reach;
    const double bottom = boxHeight + std::max({0.0, shadowY}) + reach;

    // Il centro del riquadro del testo cade nel punto scelto del fotogramma.
    const double boxLeft = style.centerX * frameWidth - boxWidth / 2.0;
    const double boxTop = style.centerY * frameHeight - boxHeight / 2.0;
    const int originX = static_cast<int>(std::floor(boxLeft + left));
    const int originY = static_cast<int>(std::floor(boxTop + top));
    const int width = static_cast<int>(std::ceil(boxLeft + right)) - originX;
    const int height = static_cast<int>(std::ceil(boxTop + bottom)) - originY;
    if (width <= 0 || height <= 0 || static_cast<long long>(width) * height > 64'000'000LL)
        return result;

    QImage layer(width, height, QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.translate(boxLeft - originX, boxTop - originY);  // da qui in poi (0, 0) è l'angolo del riquadro del testo

        if (style.background)
            p.fillRect(QRectF(0.0, 0.0, boxWidth, boxHeight), toQt(style.backgroundColor));

        // Il testo come tracciato: serve per contorno e ombra con lo stesso disegno.
        QPainterPath path;
        QPainterPath underlines;
        double baseline = padding + metrics.ascent() + (lineHeight - metrics.lineSpacing()) / 2.0;
        for (const Line& line : lines) {
            const double x = padding
                + (style.align == core::TextAlign::Left ? 0.0
                   : style.align == core::TextAlign::Right ? textWidth - line.width
                                                           : (textWidth - line.width) / 2.0);
            path.addText(QPointF(x, baseline), font, line.text);
            if (style.underline && line.width > 0.0) {
                const double thickness = std::max(1.0, pixelSize / 14.0);
                underlines.addRect(QRectF(x, baseline + metrics.underlinePos(), line.width, thickness));
            }
            baseline += lineHeight;
        }
        path.addPath(underlines);

        p.setPen(Qt::NoPen);
        if (style.shadow) {
            p.translate(shadowX, shadowY);
            if (style.outline && outline > 0.0)  // l'ombra segue anche il contorno
                p.setPen(QPen(toQt(style.shadowColor), outline * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(toQt(style.shadowColor));
            p.drawPath(path);
            p.translate(-shadowX, -shadowY);
            p.setPen(Qt::NoPen);
        }
        if (style.outline && outline > 0.0) {
            // Il contorno sta fuori dalle lettere: si disegna prima con un tratto largo il doppio, poi il riempimento.
            p.setPen(QPen(toQt(style.outlineColor), outline * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(toQt(style.outlineColor));
            p.drawPath(path);
            p.setPen(Qt::NoPen);
        }
        p.setBrush(toQt(style.color));
        p.drawPath(path);
    }

    // L'opacità di tutta la scritta si applica alla fine, così le parti sovrapposte non si sommano.
    if (style.opacity < 1.0) {
        QImage faded(width, height, QImage::Format_ARGB32_Premultiplied);
        faded.fill(Qt::transparent);
        QPainter p(&faded);
        p.setOpacity(std::clamp(style.opacity, 0.0, 1.0));
        p.drawImage(0, 0, layer);
        p.end();
        layer = std::move(faded);
    }

    // Da ARGB32 premoltiplicato (BGRA in memoria su PC) a RGBA premoltiplicato.
    result.x = originX;
    result.y = originY;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const QRgb* src = reinterpret_cast<const QRgb*>(layer.constScanLine(y));
        std::uint8_t* dst = &result.rgba[static_cast<std::size_t>(y) * width * 4];
        for (int x = 0; x < width; ++x, dst += 4) {
            dst[0] = static_cast<std::uint8_t>(qRed(src[x]));
            dst[1] = static_cast<std::uint8_t>(qGreen(src[x]));
            dst[2] = static_cast<std::uint8_t>(qBlue(src[x]));
            dst[3] = static_cast<std::uint8_t>(qAlpha(src[x]));
        }
    }
    return result;
}

PrerenderedTextRenderer::PrerenderedTextRenderer(const core::TextTracks& tracks, int frameWidth, int frameHeight, bool draw)
{
    if (!draw)
        return;
    const QtTextRenderer renderer;
    for (const core::TextLayer& layer : tracks.layers()) {
        for (const core::TextClip& clip : layer.clips)
            images_[clip.id] = renderer.render(clip, frameWidth, frameHeight);
    }
}

core::TextImage PrerenderedTextRenderer::render(const core::TextClip& clip, int, int) const
{
    const auto found = images_.find(clip.id);
    return found != images_.end() ? found->second : core::TextImage{};
}

}  // namespace editor::app
