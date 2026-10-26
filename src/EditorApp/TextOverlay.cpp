#include "EditorApp/TextOverlay.h"

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

// La scritta senza ciò che non cambia come si disegna.
core::TextClip drawnForm(const core::TextClip& clip)
{
    core::TextClip form = clip;
    form.position = 0.0;
    form.length = 1.0;
    form.fadeIn = 0.0;
    form.fadeOut = 0.0;
    return form;
}

}  // namespace

QRectF TextOverlay::bounds(const core::TextClip& clip, int frameWidth, int frameHeight) const
{
    const core::TextImage text = renderer_.render(clip, frameWidth, frameHeight);
    if (text.width <= 0 || text.height <= 0)
        return {};
    return {static_cast<double>(text.x) / frameWidth, static_cast<double>(text.y) / frameHeight,
            static_cast<double>(text.width) / frameWidth, static_cast<double>(text.height) / frameHeight};
}

void TextOverlay::paint(QPainter& painter, const QRectF& target, qreal devicePixelRatio, const std::vector<core::TextClip>& clips,
                        double time)
{
    if (clips.empty() || target.isEmpty())
        return;
    const QSize size(std::max(1, static_cast<int>(std::lround(target.width() * devicePixelRatio))),
                     std::max(1, static_cast<int>(std::lround(target.height() * devicePixelRatio))));
    // Le immagini delle scritte che non si vedono più si buttano (sono grandi quanto il testo, ma sono tante se si scorre).
    for (auto it = entries_.begin(); it != entries_.end();) {
        const bool visible = std::any_of(clips.begin(), clips.end(), [&](const core::TextClip& c) { return c.id == it->first; });
        it = visible || entries_.size() <= 8 ? std::next(it) : entries_.erase(it);
    }
    const qreal opacityBefore = painter.opacity();
    for (const core::TextClip& clip : clips) {
        const core::TextClip form = drawnForm(clip);
        Entry& entry = entries_[clip.id];
        if (entry.size != size || !(entry.clip == form) || entry.image.isNull()) {
            const core::TextImage text = renderer_.render(clip, size.width(), size.height());
            entry.clip = form;
            entry.size = size;
            entry.origin = QPoint(text.x, text.y);
            entry.image = QImage();
            if (text.width > 0 && text.height > 0) {
                // RGBA premoltiplicato: Qt lo legge direttamente (le righe sono già senza spazi); si copia perché i dati sono di `text`.
                entry.image = QImage(text.rgba.data(), text.width, text.height, text.width * 4, QImage::Format_RGBA8888_Premultiplied).copy();
            }
            ++renderCount_;
        }
        const double fade = clip.fadeFactor(time);
        if (entry.image.isNull() || fade <= 0.0)
            continue;
        painter.setOpacity(opacityBefore * fade);
        const QRectF where(target.left() + entry.origin.x() / devicePixelRatio, target.top() + entry.origin.y() / devicePixelRatio,
                           entry.image.width() / devicePixelRatio, entry.image.height() / devicePixelRatio);
        painter.drawImage(where, entry.image);
    }
    painter.setOpacity(opacityBefore);
}

}  // namespace editor::app
