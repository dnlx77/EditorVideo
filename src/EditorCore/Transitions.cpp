#include "EditorCore/Transitions.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace editor::core {

namespace {

const std::uint8_t* rowOf(const RgbaView& v, int y)
{
    return v.data + static_cast<std::ptrdiff_t>(y) * v.stride;
}

std::uint8_t* rowOf(const RgbaTarget& t, int y)
{
    return t.data + static_cast<std::ptrdiff_t>(y) * t.stride;
}

double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

// Combina due fotogrammi pixel per pixel: `weightTo` (0..256) dice quanto pesa `to`, `weightFrom` quanto pesa `from`
// (0..256, di solito 256 - weightTo; nella dissolvenza al nero no).
void mixFrames(const RgbaView& from, const RgbaView& to, int weightFrom, int weightTo, RgbaTarget out)
{
    for (int y = 0; y < from.height; ++y) {
        const std::uint8_t* a = rowOf(from, y);
        const std::uint8_t* b = rowOf(to, y);
        std::uint8_t* o = rowOf(out, y);
        for (int x = 0; x < from.width; ++x, a += 4, b += 4, o += 4) {
            for (int c = 0; c < 3; ++c)
                o[c] = static_cast<std::uint8_t>(std::min(255, (a[c] * weightFrom + b[c] * weightTo + 128) >> 8));
            o[3] = a[3];
        }
    }
}

class Crossfade final : public Transition
{
public:
    const char* id() const override { return "dissolvenza"; }
    const char* name() const override { return "Dissolvenza incrociata"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const int w = static_cast<int>(std::lround(clamp01(progress) * 256.0));
        mixFrames(from, to, 256 - w, w, out);
    }
};

// Il primo fotogramma sfuma al nero nella prima metà, poi dal nero compare il secondo.
class DipToBlack final : public Transition
{
public:
    const char* id() const override { return "dissolvenza-nero"; }
    const char* name() const override { return "Dissolvenza al nero"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const double p = clamp01(progress);
        if (p < 0.5)
            mixFrames(from, to, static_cast<int>(std::lround((1.0 - 2.0 * p) * 256.0)), 0, out);
        else
            mixFrames(from, to, 0, static_cast<int>(std::lround((2.0 * p - 1.0) * 256.0)), out);
    }
};

// Il secondo fotogramma entra spingendo il primo.
class Slide final : public Transition
{
public:
    const char* id() const override { return "scorrimento"; }
    const char* name() const override { return "Scorrimento"; }
    bool usesDirection() const override { return true; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams& params,
               RgbaTarget out) const override
    {
        const int w = from.width;
        const int h = from.height;
        const double p = clamp01(progress);
        const bool horizontal = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Right;
        const bool forward = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Up;
        const int size = horizontal ? w : h;
        const int shift = std::clamp(static_cast<int>(std::lround(p * size)), 0, size);  // di quanto è andato il primo
        // Se `out` è uno degli ingressi si passa per una copia, per non leggere pixel già scritti.
        std::vector<std::uint8_t> copy;
        const bool inPlace = out.data == from.data || out.data == to.data;
        RgbaTarget dest = out;
        if (inPlace) {
            copy.resize(static_cast<std::size_t>(w) * h * 4);
            dest = {copy.data(), w * 4};
        }
        for (int y = 0; y < h; ++y) {
            std::uint8_t* o = rowOf(dest, y);
            if (horizontal) {
                // forward (Left): il pixel x mostra from(x + shift) se x < w - shift, altrimenti to(x - (w - shift)).
                for (int x = 0; x < w; ++x) {
                    const int pos = forward ? x : w - 1 - x;
                    const bool fromSide = pos < w - shift;
                    const int sx = fromSide ? pos + shift : pos - (w - shift);
                    const int srcX = forward ? sx : w - 1 - sx;
                    const std::uint8_t* s = (fromSide ? rowOf(from, y) : rowOf(to, y)) + static_cast<std::ptrdiff_t>(srcX) * 4;
                    std::memcpy(o + static_cast<std::ptrdiff_t>(x) * 4, s, 3);
                    o[static_cast<std::ptrdiff_t>(x) * 4 + 3] = rowOf(from, y)[static_cast<std::ptrdiff_t>(x) * 4 + 3];
                }
            } else {
                const int pos = forward ? y : h - 1 - y;
                const bool fromSide = pos < h - shift;
                const int sy = fromSide ? pos + shift : pos - (h - shift);
                const int srcY = forward ? sy : h - 1 - sy;
                const std::uint8_t* s = fromSide ? rowOf(from, srcY) : rowOf(to, srcY);
                const std::uint8_t* alpha = rowOf(from, y);
                for (int x = 0; x < w; ++x) {
                    std::memcpy(o + static_cast<std::ptrdiff_t>(x) * 4, s + static_cast<std::ptrdiff_t>(x) * 4, 3);
                    o[static_cast<std::ptrdiff_t>(x) * 4 + 3] = alpha[static_cast<std::ptrdiff_t>(x) * 4 + 3];
                }
            }
        }
        if (inPlace)
            for (int y = 0; y < h; ++y)
                std::memcpy(rowOf(out, y), copy.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
    }
};

}  // namespace

const std::vector<const Transition*>& transitionRegistry()
{
    static const Crossfade crossfade;
    static const DipToBlack dipToBlack;
    static const Slide slide;
    static const std::vector<const Transition*> all = {&crossfade, &dipToBlack, &slide};
    return all;
}

const Transition* findTransition(const std::string& id)
{
    for (const Transition* t : transitionRegistry())
        if (id == t->id())
            return t;
    return nullptr;
}

}  // namespace editor::core
