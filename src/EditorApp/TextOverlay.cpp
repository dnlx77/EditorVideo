#include "EditorApp/TextOverlay.h"

#include <cmath>

namespace editor::app {

void TextOverlay::paint(QPainter& painter, const QRectF& target, qreal devicePixelRatio, const std::vector<core::TextClip>& clips)
{
    if (clips.empty() || target.isEmpty())
        return;
    const QSize size(std::max(1, static_cast<int>(std::lround(target.width() * devicePixelRatio))),
                     std::max(1, static_cast<int>(std::lround(target.height() * devicePixelRatio))));
    if (size != cachedSize_ || clips != cachedClips_ || cached_.isNull()) {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter inner(&image);
        for (const core::TextClip& clip : clips) {
            const core::TextImage text = renderer_.render(clip, size.width(), size.height());
            if (text.width <= 0 || text.height <= 0)
                continue;
            // RGBA premoltiplicato: Qt lo legge direttamente (le righe sono già senza spazi).
            const QImage part(text.rgba.data(), text.width, text.height, text.width * 4, QImage::Format_RGBA8888_Premultiplied);
            inner.drawImage(text.x, text.y, part);
        }
        inner.end();
        cached_ = image;
        cachedClips_ = clips;
        cachedSize_ = size;
        ++renderCount_;
    }
    painter.drawImage(target, cached_);
}

}  // namespace editor::app
