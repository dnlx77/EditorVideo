#include "EditorCore/TextImage.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace editor::core {

void blendTextOver(std::uint8_t* frameRgba, int stride, int frameWidth, int frameHeight, const TextImage& text, double opacity)
{
    if (frameRgba == nullptr || text.empty() || text.rgba.size() < static_cast<std::size_t>(text.width) * text.height * 4)
        return;
    const int op = std::clamp(static_cast<int>(std::lround(opacity * 256.0)), 0, 256);
    if (op == 0)
        return;
    const int x0 = std::max(text.x, 0);
    const int y0 = std::max(text.y, 0);
    const int x1 = std::min(text.x + text.width, frameWidth);
    const int y1 = std::min(text.y + text.height, frameHeight);
    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* src = &text.rgba[(static_cast<std::size_t>(y - text.y) * text.width + (x0 - text.x)) * 4];
        std::uint8_t* dst = frameRgba + static_cast<std::ptrdiff_t>(y) * stride + static_cast<std::ptrdiff_t>(x0) * 4;
        for (int x = x0; x < x1; ++x, src += 4, dst += 4) {
            const int alpha = (src[3] * op) >> 8;
            const int inverse = 255 - alpha;
            if (inverse == 255)
                continue;  // trasparente
            for (int c = 0; c < 3; ++c)
                dst[c] = static_cast<std::uint8_t>(std::min(255, ((src[c] * op) >> 8) + (dst[c] * inverse + 127) / 255));
        }
    }
}

TextImage transformTextImage(const TextImage& text, const Transform& transform, double pivotX, double pivotY, int frameWidth,
                             int frameHeight)
{
    const Transform t = normalized(transform);
    if (text.empty() || !t.movesPicture() || frameWidth <= 0 || frameHeight <= 0
        || text.rgba.size() < static_cast<std::size_t>(text.width) * text.height * 4)
        return text;
    const double ex = t.scaleX * (t.flipHorizontal ? -1.0 : 1.0);
    const double ey = t.scaleY * (t.flipVertical ? -1.0 : 1.0);
    const double radians = t.rotation * std::numbers::pi / 180.0;
    const double cs = std::cos(radians);
    const double sn = std::sin(radians);
    // p' = pivot + spostamento + R * D * (p - pivot); con y verso il basso un angolo positivo gira in senso orario.
    Affine forward;
    forward.a = cs * ex;
    forward.b = sn * ex;
    forward.c = -sn * ey;
    forward.d = cs * ey;
    forward.tx = pivotX + t.posX - (forward.a * pivotX + forward.c * pivotY);
    forward.ty = pivotY + t.posY - (forward.b * pivotX + forward.d * pivotY);
    const Affine inverse = forward.inverse();

    const double left = text.x;
    const double top = text.y;
    const double right = text.x + text.width;
    const double bottom = text.y + text.height;
    const Point2 corners[4] = {forward.apply(left, top), forward.apply(right, top), forward.apply(right, bottom),
                               forward.apply(left, bottom)};
    double minX = corners[0].x, maxX = corners[0].x, minY = corners[0].y, maxY = corners[0].y;
    for (const Point2& p : corners) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(minX)) - 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(minY)) - 1);
    const int x1 = std::min(frameWidth, static_cast<int>(std::ceil(maxX)) + 1);
    const int y1 = std::min(frameHeight, static_cast<int>(std::ceil(maxY)) + 1);
    TextImage out;
    if (x1 <= x0 || y1 <= y0)
        return out;
    out.x = x0;
    out.y = y0;
    out.width = x1 - x0;
    out.height = y1 - y0;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    const auto texel = [&](int ix, int iy, int c) -> double {
        if (ix < 0 || iy < 0 || ix >= text.width || iy >= text.height)
            return 0.0;  // fuori dal riquadro: trasparente (così i bordi sono morbidi)
        return text.rgba[(static_cast<std::size_t>(iy) * text.width + ix) * 4 + static_cast<std::size_t>(c)];
    };
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const Point2 s = inverse.apply(x0 + x + 0.5, y0 + y + 0.5);
            const double u = s.x - text.x - 0.5;
            const double v = s.y - text.y - 0.5;
            if (u <= -1.0 || v <= -1.0 || u >= text.width || v >= text.height)
                continue;
            const int iu = static_cast<int>(std::floor(u));
            const int iv = static_cast<int>(std::floor(v));
            const double fu = u - iu;
            const double fv = v - iv;
            std::uint8_t* dst = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            for (int c = 0; c < 4; ++c) {
                const double value = (texel(iu, iv, c) * (1 - fu) + texel(iu + 1, iv, c) * fu) * (1 - fv)
                                     + (texel(iu, iv + 1, c) * (1 - fu) + texel(iu + 1, iv + 1, c) * fu) * fv;
                dst[c] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
            }
        }
    }
    return out;
}

}  // namespace editor::core
