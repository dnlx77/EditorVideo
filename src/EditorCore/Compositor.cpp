#include "EditorCore/Compositor.h"

#include "EditorCore/ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace editor::core {

namespace {

// Riduce `source` di un fattore intero `k` (media di blocchi k x k); le righe e colonne che avanzano si scartano.
std::vector<std::uint8_t> boxReduce(const BgraImage& source, int k, int& outWidth, int& outHeight)
{
    outWidth = std::max(1, source.width / k);
    outHeight = std::max(1, source.height / k);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(outWidth) * outHeight * 4);
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            int sum[3] = {0, 0, 0};
            int count = 0;
            for (int dy = 0; dy < k; ++dy) {
                const int sy = y * k + dy;
                if (sy >= source.height)
                    break;
                const std::uint8_t* row = source.data + static_cast<std::ptrdiff_t>(sy) * source.stride;
                for (int dx = 0; dx < k; ++dx) {
                    const int sx = x * k + dx;
                    if (sx >= source.width)
                        break;
                    for (int c = 0; c < 3; ++c)
                        sum[c] += row[sx * 4 + c];
                    ++count;
                }
            }
            std::uint8_t* px = &out[(static_cast<std::size_t>(y) * outWidth + x) * 4];
            for (int c = 0; c < 3; ++c)
                px[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / std::max(count, 1));
            px[3] = 255;
        }
    }
    return out;
}

float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

void compositeLayer(std::uint8_t* canvas, int canvasStride, int canvasWidth, int canvasHeight, const BgraImage& source,
                    const Transform& transform)
{
    if (canvas == nullptr || source.data == nullptr || canvasWidth <= 0 || canvasHeight <= 0 || source.width <= 0 ||
        source.height <= 0)
        return;
    const Transform t = normalized(transform);
    if (t.opacity <= 0.0)
        return;

    BgraImage image = source;
    TransformGeometry g = computeGeometry(t, image.width, image.height, canvasWidth, canvasHeight);
    // Rimpicciolito molto: prima una media di blocchi, poi l'interpolazione bilineare fa il resto.
    const double scale = std::sqrt(std::abs(g.toProject.a * g.toProject.d - g.toProject.b * g.toProject.c));
    std::vector<std::uint8_t> reduced;
    if (scale > 0.0 && scale < 0.5) {
        const int k = std::min(static_cast<int>(std::floor(1.0 / scale)), std::min(image.width, image.height));
        if (k >= 2) {
            int w = 0;
            int h = 0;
            reduced = boxReduce(image, k, w, h);
            image = {reduced.data(), w * 4, w, h};
            g = computeGeometry(t, w, h, canvasWidth, canvasHeight);
        }
    }

    // L'area dello schermo toccata dal pezzo.
    const PixelBounds box = pixelBounds(g.corners, canvasWidth, canvasHeight);
    if (box.empty())
        return;
    const int x0 = box.x0;
    const int x1 = box.x1;
    const int y0 = box.y0;
    const int y1 = box.y1;

    const Affine inv = g.toProject.inverse();
    // Quanto è lungo sullo schermo un pixel del file nelle due direzioni (per sfumare i bordi di mezzo pixel dello schermo).
    const float lenX = static_cast<float>(std::hypot(g.toProject.a, g.toProject.b));
    const float lenY = static_cast<float>(std::hypot(g.toProject.c, g.toProject.d));
    const float left = static_cast<float>(g.sourceLeft);
    const float right = static_cast<float>(g.sourceRight);
    const float top = static_cast<float>(g.sourceTop);
    const float bottom = static_cast<float>(g.sourceBottom);
    // I pixel che si possono leggere (restano dentro la parte visibile, così non entra il colore di quello ritagliato).
    const int minIx = std::clamp(static_cast<int>(std::floor(left)), 0, image.width - 1);
    const int maxIx = std::clamp(static_cast<int>(std::ceil(right)) - 1, minIx, image.width - 1);
    const int minIy = std::clamp(static_cast<int>(std::floor(top)), 0, image.height - 1);
    const int maxIy = std::clamp(static_cast<int>(std::ceil(bottom)) - 1, minIy, image.height - 1);
    const float opacity = static_cast<float>(t.opacity);

    // Ogni pixel dipende solo dalla sua posizione: le righe si dividono tra più thread senza cambiare il risultato.
    // L'interpolazione usa pesi interi a 8 bit (i valori differiscono al più di un livello dal calcolo in virgola mobile;
    // con spostamenti interi e pezzo opaco sono esatti) e la posizione nel file avanza per somme lungo la riga.
    const int alphaScale = static_cast<int>(std::lround(opacity * 256.0f));
    const auto drawRows = [&](int rowStart, int rowEnd) {
        for (int y = rowStart; y < rowEnd; ++y) {
            std::uint8_t* dst = canvas + static_cast<std::ptrdiff_t>(y) * canvasStride + static_cast<std::ptrdiff_t>(x0) * 4;
            const Point2 first = inv.apply(x0 + 0.5, y + 0.5);
            double pu = first.x;
            double pv = first.y;
            for (int x = x0; x < x1; ++x, dst += 4, pu += inv.a, pv += inv.b) {
                const float u = static_cast<float>(pu);
                const float v = static_cast<float>(pv);
                const float coverage = clamp01((u - left) * lenX + 0.5f) * clamp01((right - u) * lenX + 0.5f) *
                                       clamp01((v - top) * lenY + 0.5f) * clamp01((bottom - v) * lenY + 0.5f);
                if (coverage <= 0.0f)
                    continue;
                // Bilineare: i centri dei pixel sono a +0.5.
                const float fx = u - 0.5f;
                const float fy = v - 0.5f;
                const int flx = static_cast<int>(fx) - (fx < 0.0f ? 1 : 0);  // pavimento
                const int fly = static_cast<int>(fy) - (fy < 0.0f ? 1 : 0);
                const int wx = static_cast<int>((fx - static_cast<float>(flx)) * 256.0f);  // 0..255
                const int wy = static_cast<int>((fy - static_cast<float>(fly)) * 256.0f);
                const int ix0 = std::clamp(flx, minIx, maxIx);
                const int ix1 = std::clamp(flx + 1, minIx, maxIx);
                const int iy0 = std::clamp(fly, minIy, maxIy);
                const int iy1 = std::clamp(fly + 1, minIy, maxIy);
                const std::uint8_t* p00 = image.data + static_cast<std::ptrdiff_t>(iy0) * image.stride + ix0 * 4;
                const std::uint8_t* p01 = image.data + static_cast<std::ptrdiff_t>(iy0) * image.stride + ix1 * 4;
                const std::uint8_t* p10 = image.data + static_cast<std::ptrdiff_t>(iy1) * image.stride + ix0 * 4;
                const std::uint8_t* p11 = image.data + static_cast<std::ptrdiff_t>(iy1) * image.stride + ix1 * 4;
                const int alpha = std::clamp(static_cast<int>(coverage * static_cast<float>(alphaScale) + 0.5f), 0, 256);
                for (int c = 0; c < 3; ++c) {
                    const int top2 = p00[c] * (256 - wx) + p01[c] * wx;
                    const int bot2 = p10[c] * (256 - wx) + p11[c] * wx;
                    const int color = (top2 * (256 - wy) + bot2 * wy + 32768) >> 16;
                    dst[c] = static_cast<std::uint8_t>(alpha >= 256 ? color : (color * alpha + dst[c] * (256 - alpha) + 128) >> 8);
                }
                dst[3] = 255;
            }
        }
    };
    const int rows = y1 - y0;
    const std::int64_t area = static_cast<std::int64_t>(rows) * (x1 - x0);
    if (area < 100000) {
        drawRows(y0, y1);
        return;
    }
    // Blocchi di righe presi a turno (le righe di un pezzo ruotato non costano tutte uguale).
    constexpr int kBlock = 16;
    parallelFor((rows + kBlock - 1) / kBlock, [&](int block) {
        const int start = y0 + block * kBlock;
        drawRows(start, std::min(start + kBlock, y1));
    });
}

}  // namespace editor::core
