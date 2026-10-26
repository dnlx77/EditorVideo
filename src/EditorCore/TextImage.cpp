#include "EditorCore/TextImage.h"

#include <algorithm>
#include <cmath>

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

}  // namespace editor::core
