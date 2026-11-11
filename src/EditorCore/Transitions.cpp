#include "EditorCore/Transitions.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace editor::core {

double wipeFeatherPixels(int size)
{
    return std::max(2.0, size * 0.04);
}

namespace {

const std::uint8_t* rowOf(const RgbaView& v, int y)
{
    return v.data + static_cast<std::ptrdiff_t>(y) * v.stride;
}

std::uint8_t* rowOf(const RgbaTarget& t, int y)
{
    return t.data + static_cast<std::ptrdiff_t>(y) * t.stride;
}

// Combina due fotogrammi pixel per pixel: `weightTo` (0..256) dice quanto pesa `to`, `weightFrom` quanto pesa `from`
// (0..256, di solito 256 - weightTo; nelle dissolvenze al nero e al bianco no); `weightWhite` (0..256) quanto pesa il bianco.
void mixFrames(const RgbaView& from, const RgbaView& to, int weightFrom, int weightTo, RgbaTarget out, int weightWhite = 0)
{
    for (int y = 0; y < from.height; ++y) {
        const std::uint8_t* a = rowOf(from, y);
        const std::uint8_t* b = rowOf(to, y);
        std::uint8_t* o = rowOf(out, y);
        for (int x = 0; x < from.width; ++x, a += 4, b += 4, o += 4) {
            for (int c = 0; c < 3; ++c)
                o[c] = static_cast<std::uint8_t>(std::min(255, (a[c] * weightFrom + b[c] * weightTo + 255 * weightWhite + 128) >> 8));
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
        const int w = static_cast<int>(std::lround(std::clamp(progress, 0.0, 1.0) * 256.0));
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
        const double p = std::clamp(progress, 0.0, 1.0);
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
        const double p = std::clamp(progress, 0.0, 1.0);
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

// Come la dissolvenza al nero, ma passando dal bianco.
class DipToWhite final : public Transition
{
public:
    const char* id() const override { return "dissolvenza-bianco"; }
    const char* name() const override { return "Dissolvenza al bianco"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const double p = std::clamp(progress, 0.0, 1.0);
        if (p < 0.5) {
            const int white = static_cast<int>(std::lround(2.0 * p * 256.0));
            mixFrames(from, to, 256 - white, 0, out, white);
        } else {
            const int toWeight = static_cast<int>(std::lround((2.0 * p - 1.0) * 256.0));
            mixFrames(from, to, 0, toWeight, out, 256 - toWeight);
        }
    }
};

// Il secondo fotogramma compare dietro un bordo che attraversa il quadro (Left = il bordo va verso sinistra, il secondo
// entra da destra). Il bordo è sfumato su ~4% della misura.
class Wipe final : public Transition
{
public:
    const char* id() const override { return "tendina"; }
    const char* name() const override { return "Tendina"; }
    bool usesDirection() const override { return true; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams& params,
               RgbaTarget out) const override
    {
        const int w = from.width;
        const int h = from.height;
        const double p = std::clamp(progress, 0.0, 1.0);
        const bool horizontal = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Right;
        const bool forward = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Up;
        const int size = horizontal ? w : h;
        const double feather = wipeFeatherPixels(size);
        const double edge = p * (size + feather) - feather / 2.0;  // posizione del centro del bordo
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* a = rowOf(from, y);
            const std::uint8_t* b = rowOf(to, y);
            std::uint8_t* o = rowOf(out, y);
            for (int x = 0; x < w; ++x, a += 4, b += 4, o += 4) {
                const int c = horizontal ? x : y;
                const double d = (forward ? size - 1 - c : c) + 0.5;  // distanza dal lato da cui entra il secondo
                const double weight = std::clamp((edge - d) / feather + 0.5, 0.0, 1.0);
                const int wt = static_cast<int>(std::lround(weight * 256.0));
                for (int k = 0; k < 3; ++k)
                    o[k] = static_cast<std::uint8_t>(std::min(255, (a[k] * (256 - wt) + b[k] * wt + 128) >> 8));
                o[3] = a[3];
            }
        }
    }

};

// Scorrimento in cui si muove un solo fotogramma: copertura (il secondo entra sopra il primo, fermo) o scopertura (il
// primo esce e scopre il secondo, fermo). Direzioni come lo scorrimento.
class CoverUncover final : public Transition
{
public:
    CoverUncover(const char* id, const char* name, bool cover) : id_(id), name_(name), cover_(cover) {}
    const char* id() const override { return id_; }
    const char* name() const override { return name_; }
    bool usesDirection() const override { return true; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams& params,
               RgbaTarget out) const override
    {
        const int w = from.width;
        const int h = from.height;
        const double p = std::clamp(progress, 0.0, 1.0);
        const bool horizontal = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Right;
        const bool forward = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Up;
        const int size = horizontal ? w : h;
        const int shift = std::clamp(static_cast<int>(std::lround(p * size)), 0, size);
        std::vector<std::uint8_t> copy(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int c = horizontal ? x : y;
                const int pos = forward ? c : size - 1 - c;
                const bool fromSide = pos < size - shift;
                int src = pos;
                if (fromSide && !cover_)
                    src = pos + shift;
                else if (!fromSide && cover_)
                    src = pos - (size - shift);
                const int srcC = forward ? src : size - 1 - src;
                const int sx = horizontal ? srcC : x;
                const int sy = horizontal ? y : srcC;
                const std::uint8_t* s = (fromSide ? rowOf(from, sy) : rowOf(to, sy)) + static_cast<std::ptrdiff_t>(sx) * 4;
                std::uint8_t* o = copy.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                std::memcpy(o, s, 3);
                o[3] = rowOf(from, y)[static_cast<std::ptrdiff_t>(x) * 4 + 3];
            }
        for (int y = 0; y < h; ++y)
            std::memcpy(rowOf(out, y), copy.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
    }

private:
    const char* id_;
    const char* name_;
    bool cover_;
};

}  // namespace

const std::vector<const Transition*>& transitionRegistry()
{
    static const Crossfade crossfade;
    static const DipToBlack dipToBlack;
    static const Slide slide;
    static const DipToWhite dipToWhite;
    static const Wipe wipe;
    static const CoverUncover cover("copertura", "Copertura", true);
    static const CoverUncover uncover("scopertura", "Scopertura", false);
    static const std::vector<const Transition*> all = {&crossfade, &dipToBlack, &dipToWhite, &slide, &wipe, &cover, &uncover};
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
