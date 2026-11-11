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

int blindsBandWidth(int size)
{
    return std::max(1, static_cast<int>(std::lround(size / 8.0)));
}

int mosaicBlockSize(double progress, int width, int height)
{
    const double p = std::clamp(progress, 0.0, 1.0);
    const double q = p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p;
    const int maxBlock = std::max(2, std::min(width, height) / 8);
    return 1 + static_cast<int>(std::lround(q * (maxBlock - 1)));
}

double irisMaxRadius(int width, int height)
{
    return 0.5 * std::sqrt(static_cast<double>(width) * width + static_cast<double>(height) * height);
}

double irisFeatherPixels(int width, int height)
{
    return std::max(2.0, std::min(width, height) * 0.04);
}

namespace {

constexpr double kTwoPi = 6.283185307179586;

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

// Peso (0..1) del secondo fotogramma nel pixel (x, y) per gli effetti che hanno un bordo sfumato; poi mescolati in 8 bit.
void mixByWeight(const RgbaView& from, const RgbaView& to, RgbaTarget out, double (*weightAt)(int, int, int, int, double, double),
                 double edge, double feather)
{
    for (int y = 0; y < from.height; ++y) {
        const std::uint8_t* a = rowOf(from, y);
        const std::uint8_t* b = rowOf(to, y);
        std::uint8_t* o = rowOf(out, y);
        for (int x = 0; x < from.width; ++x, a += 4, b += 4, o += 4) {
            const int wt = static_cast<int>(std::lround(weightAt(x, y, from.width, from.height, edge, feather) * 256.0));
            for (int k = 0; k < 3; ++k)
                o[k] = static_cast<std::uint8_t>(std::min(255, (a[k] * (256 - wt) + b[k] * wt + 128) >> 8));
            o[3] = a[3];
        }
    }
}

double irisWeight(int x, int y, int w, int h, double edge, double feather)
{
    const double dx = x + 0.5 - w / 2.0;
    const double dy = y + 0.5 - h / 2.0;
    return std::clamp((edge - std::sqrt(dx * dx + dy * dy)) / feather + 0.5, 0.0, 1.0);
}

double clockWeight(int x, int y, int w, int h, double edge, double feather)
{
    const double dx = x + 0.5 - w / 2.0;
    const double dy = y + 0.5 - h / 2.0;
    double angle = (dx * dx + dy * dy < 1e-8) ? 0.0 : std::atan2(dx, -dy);  // da ore 12, in senso orario
    if (angle < 0.0)
        angle += kTwoPi;
    return std::clamp((edge - angle) / feather + 0.5, 0.0, 1.0);
}

// Un cerchio che si apre dal centro e scopre il secondo fotogramma.
class Iris final : public Transition
{
public:
    const char* id() const override { return "iris"; }
    const char* name() const override { return "Iris"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const double p = std::clamp(progress, 0.0, 1.0);
        const double radius = irisMaxRadius(from.width, from.height);
        const double feather = irisFeatherPixels(from.width, from.height);
        mixByWeight(from, to, out, irisWeight, p * (radius + feather) - feather / 2.0, feather);
    }
};

// Una lancetta che gira in senso orario da ore 12 e scopre il secondo fotogramma.
class Clock final : public Transition
{
public:
    const char* id() const override { return "orologio"; }
    const char* name() const override { return "Orologio"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const double p = std::clamp(progress, 0.0, 1.0);
        mixByWeight(from, to, out, clockWeight, p * (kTwoPi + kClockFeather) - kClockFeather / 2.0, kClockFeather);
    }
};

// Il primo fotogramma si ingrandisce (da 1x a 2x, dal centro) mentre sfuma nel secondo.
class Zoom final : public Transition
{
public:
    const char* id() const override { return "zoom"; }
    const char* name() const override { return "Zoom"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const int w = from.width;
        const int h = from.height;
        const double p = std::clamp(progress, 0.0, 1.0);
        const double scale = 1.0 + p;
        const int wt = static_cast<int>(std::lround(p * 256.0));
        std::vector<std::uint8_t> copy(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            const double sy = (y + 0.5 - h / 2.0) / scale + h / 2.0 - 0.5;
            const int y0 = static_cast<int>(std::floor(sy));
            const int wy = static_cast<int>(std::lround((sy - y0) * 256.0));
            const int ya = std::clamp(y0, 0, h - 1);
            const int yb = std::clamp(y0 + 1, 0, h - 1);
            for (int x = 0; x < w; ++x) {
                const double sx = (x + 0.5 - w / 2.0) / scale + w / 2.0 - 0.5;
                const int x0 = static_cast<int>(std::floor(sx));
                const int wx = static_cast<int>(std::lround((sx - x0) * 256.0));
                const int xa = std::clamp(x0, 0, w - 1);
                const int xb = std::clamp(x0 + 1, 0, w - 1);
                const std::uint8_t* p00 = rowOf(from, ya) + static_cast<std::ptrdiff_t>(xa) * 4;
                const std::uint8_t* p10 = rowOf(from, ya) + static_cast<std::ptrdiff_t>(xb) * 4;
                const std::uint8_t* p01 = rowOf(from, yb) + static_cast<std::ptrdiff_t>(xa) * 4;
                const std::uint8_t* p11 = rowOf(from, yb) + static_cast<std::ptrdiff_t>(xb) * 4;
                const std::uint8_t* b = rowOf(to, y) + static_cast<std::ptrdiff_t>(x) * 4;
                std::uint8_t* o = copy.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                for (int k = 0; k < 3; ++k) {
                    const int zoomed = (p00[k] * (256 - wx) * (256 - wy) + p10[k] * wx * (256 - wy) + p01[k] * (256 - wx) * wy
                                        + p11[k] * wx * wy + 32768) >> 16;
                    o[k] = static_cast<std::uint8_t>(std::min(255, (zoomed * (256 - wt) + b[k] * wt + 128) >> 8));
                }
                o[3] = rowOf(from, y)[static_cast<std::ptrdiff_t>(x) * 4 + 3];
            }
        }
        for (int y = 0; y < h; ++y)
            std::memcpy(rowOf(out, y), copy.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
    }
};

// Il quadro è diviso in 8 strisce (verticali con Left/Right, orizzontali con Up/Down); in ognuna il secondo fotogramma
// cresce dal lato da cui entra (come la tendina), con bordo netto.
class Blinds final : public Transition
{
public:
    const char* id() const override { return "persiana"; }
    const char* name() const override { return "Persiana"; }
    bool usesDirection() const override { return true; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams& params,
               RgbaTarget out) const override
    {
        const int w = from.width;
        const int h = from.height;
        const double p = std::clamp(progress, 0.0, 1.0);
        const bool horizontal = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Right;
        const bool forward = params.direction == TransitionDirection::Left || params.direction == TransitionDirection::Up;
        const int band = blindsBandWidth(horizontal ? w : h);
        const int reveal = static_cast<int>(std::lround(p * band));
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* a = rowOf(from, y);
            const std::uint8_t* b = rowOf(to, y);
            std::uint8_t* o = rowOf(out, y);
            for (int x = 0; x < w; ++x, a += 4, b += 4, o += 4) {
                const int inBand = (horizontal ? x : y) % band;
                const int offset = forward ? band - 1 - inBand : inBand;
                const std::uint8_t* s = offset < reveal ? b : a;
                const std::uint8_t alpha = a[3];
                std::memmove(o, s, 3);
                o[3] = alpha;
            }
        }
    }
};

// L'immagine si pixelizza a blocchi sempre più grandi, poi (a metà) compare il secondo fotogramma a blocchi che si rimpiccioliscono.
class Mosaic final : public Transition
{
public:
    const char* id() const override { return "mosaico"; }
    const char* name() const override { return "Mosaico"; }
    void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams&, RgbaTarget out) const override
    {
        const int w = from.width;
        const int h = from.height;
        const double p = std::clamp(progress, 0.0, 1.0);
        const int block = mosaicBlockSize(p, w, h);
        const RgbaView& source = p < 0.5 ? from : to;
        std::vector<std::uint8_t> copy(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            const int sy = std::min((y / block) * block + block / 2, h - 1);
            for (int x = 0; x < w; ++x) {
                const int sx = std::min((x / block) * block + block / 2, w - 1);
                std::uint8_t* o = copy.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                std::memcpy(o, rowOf(source, sy) + static_cast<std::ptrdiff_t>(sx) * 4, 3);
                o[3] = rowOf(from, y)[static_cast<std::ptrdiff_t>(x) * 4 + 3];
            }
        }
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
    static const DipToWhite dipToWhite;
    static const Wipe wipe;
    static const CoverUncover cover("copertura", "Copertura", true);
    static const CoverUncover uncover("scopertura", "Scopertura", false);
    static const Iris iris;
    static const Clock clock;
    static const Zoom zoom;
    static const Blinds blinds;
    static const Mosaic mosaic;
    static const std::vector<const Transition*> all = {&crossfade, &dipToBlack, &dipToWhite, &slide, &wipe, &cover, &uncover, &iris, &clock, &zoom, &blinds, &mosaic};
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
