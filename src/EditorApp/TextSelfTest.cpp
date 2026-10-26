#include "EditorApp/QtTextRenderer.h"
#include "EditorApp/SelfTests.h"

#include <QtCore/QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace editor::app {

namespace {

namespace core = editor::core;

struct Ink
{
    bool any = false;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;  // nell'immagine della scritta
    int maxAlpha = 0;
    int width() const { return maxX - minX + 1; }
    int height() const { return maxY - minY + 1; }
};

// Il rettangolo dei punti non trasparenti (da rowFrom a rowTo escluso).
Ink inkOf(const core::TextImage& image, int rowFrom = 0, int rowTo = -1, int minAlpha = 1)
{
    Ink ink;
    if (rowTo < 0)
        rowTo = image.height;
    for (int y = rowFrom; y < rowTo; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int a = image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + 3];
            if (a < minAlpha)
                continue;
            if (!ink.any) {
                ink = {true, x, x, y, y, a};
            } else {
                ink.minX = std::min(ink.minX, x);
                ink.maxX = std::max(ink.maxX, x);
                ink.minY = std::min(ink.minY, y);
                ink.maxY = std::max(ink.maxY, y);
                ink.maxAlpha = std::max(ink.maxAlpha, a);
            }
        }
    }
    return ink;
}

const std::uint8_t* pixel(const core::TextImage& image, int x, int y)
{
    return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

core::TextClip clipWith(const char* text)
{
    core::TextClip clip;
    clip.text = text;
    return clip;
}

}  // namespace

// --text-test: disegna scritte con Qt e controlla il risultato a pixel (misure, colori, contorno, ombra, sfondo,
// opacità, posizione, allineamento, indipendenza dalla risoluzione e sovrapposizione al fotogramma). Codice 0 se tutto è
// giusto, 3 se un controllo non riesce.
int runTextTest(const QStringList&, qsizetype)
{
    const QtTextRenderer renderer;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "ok" : "ERRORE", what);
        if (!ok)
            ++failures;
    };
    constexpr int W = 640;
    constexpr int H = 360;

    // Testo bianco su trasparente, al centro.
    core::TextClip base = clipWith("Ciao Mondo");
    base.style.centerX = 0.5;
    base.style.centerY = 0.5;
    const core::TextImage plain = renderer.render(base, W, H);
    const Ink plainInk = inkOf(plain);
    check(!plain.empty() && plainInk.any, "il testo semplice produce un'immagine con inchiostro");
    check(plainInk.maxAlpha == 255, "l'inchiostro è pieno (alfa 255)");
    bool whiteOnly = true;
    for (std::size_t i = 0; i < plain.rgba.size(); i += 4)
        whiteOnly = whiteOnly && plain.rgba[i] == plain.rgba[i + 3] && plain.rgba[i + 1] == plain.rgba[i + 3]
            && plain.rgba[i + 2] == plain.rgba[i + 3];
    check(whiteOnly, "il testo bianco ha R=G=B=alfa (premoltiplicato)");
    const double centerX = plain.x + plainInk.minX + plainInk.width() / 2.0;
    const double centerY = plain.y + plainInk.minY + plainInk.height() / 2.0;
    check(std::abs(centerX - W / 2.0) < 6.0, "il testo è centrato in orizzontale");
    check(std::abs(centerY - H / 2.0) < 0.06 * H, "il testo è centrato in verticale");
    check(core::TextImage{}.empty() && renderer.render(clipWith(""), W, H).empty(), "un testo vuoto non disegna niente");

    // Grandezza e risoluzione.
    core::TextClip big = base;
    big.style.size = base.style.size * 2.0;
    const Ink bigInk = inkOf(renderer.render(big, W, H));
    check(bigInk.height() > plainInk.height() * 1.7 && bigInk.width() > plainInk.width() * 1.7, "grandezza doppia: il testo è circa il doppio");
    const Ink hiRes = inkOf(renderer.render(base, W * 2, H * 2));
    check(std::abs(hiRes.height() - plainInk.height() * 2.0) < plainInk.height() * 0.2
              && std::abs(hiRes.width() - plainInk.width() * 2.0) < plainInk.width() * 0.2,
          "a risoluzione doppia il testo è doppio (stessa resa)");

    // Più righe e allineamento.
    core::TextClip two = clipWith("WWWWWWWW\niii");
    two.style.centerX = 0.5;
    two.style.centerY = 0.5;
    two.style.align = core::TextAlign::Left;
    const core::TextImage left = renderer.render(two, W, H);
    check(inkOf(left).height() > plainInk.height() * 1.6, "due righe sono circa il doppio di una");
    const Ink leftLow = inkOf(left, left.height * 6 / 10);
    two.style.align = core::TextAlign::Right;
    const core::TextImage right = renderer.render(two, W, H);
    const Ink rightLow = inkOf(right, right.height * 6 / 10);
    two.style.align = core::TextAlign::Center;
    const core::TextImage mid = renderer.render(two, W, H);
    const Ink midLow = inkOf(mid, mid.height * 6 / 10);
    check(leftLow.any && leftLow.minX < left.width * 0.15, "allineato a sinistra la seconda riga parte dal bordo sinistro");
    check(rightLow.any && rightLow.maxX > right.width * 0.85, "allineato a destra la seconda riga arriva al bordo destro");
    check(midLow.any && std::abs((midLow.minX + midLow.maxX) / 2.0 - mid.width / 2.0) < mid.width * 0.08,
          "centrato la seconda riga sta in mezzo");

    // Colore.
    core::TextClip red = base;
    red.style.color = {255, 0, 0, 255};
    const core::TextImage redImage = renderer.render(red, W, H);
    bool redOk = false;
    for (std::size_t i = 0; i < redImage.rgba.size(); i += 4)
        redOk = redOk || (redImage.rgba[i] == 255 && redImage.rgba[i + 1] == 0 && redImage.rgba[i + 2] == 0);
    check(redOk, "il colore rosso si vede (255, 0, 0)");

    // Contorno.
    core::TextClip outlined = base;
    outlined.style.outline = true;
    outlined.style.outlineColor = {0, 0, 0, 255};
    outlined.style.outlineWidth = 0.01;
    const core::TextImage outlinedImage = renderer.render(outlined, W, H);
    const Ink outlinedInk = inkOf(outlinedImage);
    check(outlinedInk.width() > plainInk.width() + 4 && outlinedInk.height() > plainInk.height() + 4,
          "il contorno allarga il testo");
    bool hasBlack = false;
    bool hasWhite = false;
    for (std::size_t i = 0; i < outlinedImage.rgba.size(); i += 4) {
        const std::uint8_t* p = &outlinedImage.rgba[i];
        hasBlack = hasBlack || (p[3] == 255 && p[0] == 0 && p[1] == 0 && p[2] == 0);
        hasWhite = hasWhite || (p[3] == 255 && p[0] == 255 && p[1] == 255 && p[2] == 255);
    }
    check(hasBlack && hasWhite, "con il contorno si vedono sia il nero sia il bianco");

    // Ombra.
    core::TextClip shadowed = base;
    shadowed.style.shadow = true;
    shadowed.style.shadowColor = {0, 0, 255, 255};
    shadowed.style.shadowOffsetX = 0.02;
    shadowed.style.shadowOffsetY = 0.02;
    const core::TextImage shadowImage = renderer.render(shadowed, W, H);
    check(inkOf(shadowImage).width() > plainInk.width() + 5, "l'ombra fa sporgere il riquadro");
    bool blue = false;
    for (std::size_t i = 0; i < shadowImage.rgba.size(); i += 4)
        blue = blue || (shadowImage.rgba[i] == 0 && shadowImage.rgba[i + 1] == 0 && shadowImage.rgba[i + 2] == 255 && shadowImage.rgba[i + 3] == 255);
    check(blue, "l'ombra ha il suo colore (blu)");

    // Sfondo.
    core::TextClip boxed = base;
    boxed.style.background = true;
    boxed.style.backgroundColor = {0, 128, 0, 255};
    boxed.style.backgroundPadding = 0.02;
    const core::TextImage boxedImage = renderer.render(boxed, W, H);
    const std::uint8_t* corner = pixel(boxedImage, 1, 1);
    check(boxedImage.width > plain.width + 10 && corner[0] == 0 && corner[1] == 128 && corner[2] == 0 && corner[3] == 255,
          "lo sfondo riempie il riquadro con il suo colore");
    boxed.style.backgroundColor = {0, 128, 0, 128};
    const std::uint8_t* half = pixel(renderer.render(boxed, W, H), 1, 1);
    check(half[3] == 128 && half[1] <= 128, "lo sfondo semitrasparente è premoltiplicato");

    // Opacità.
    core::TextClip faded = base;
    faded.style.opacity = 0.5;
    const Ink fadedInk = inkOf(renderer.render(faded, W, H));
    check(fadedInk.maxAlpha >= 126 && fadedInk.maxAlpha <= 130, "opacità 0,5: alfa circa 128");

    // Posizione: in alto a sinistra.
    core::TextClip corner2 = base;
    corner2.style.centerX = 0.2;
    corner2.style.centerY = 0.1;
    const core::TextImage cornerImage = renderer.render(corner2, W, H);
    const Ink cornerInk = inkOf(cornerImage);
    check(cornerImage.x + cornerInk.minX + cornerInk.width() / 2.0 < W * 0.25
              && cornerImage.y + cornerInk.minY + cornerInk.height() / 2.0 < H * 0.2,
          "la posizione sposta la scritta (20% / 10%)");

    // Sottolineato: più inchiostro in basso rispetto al testo normale.
    core::TextClip underlined = base;
    underlined.style.underline = true;
    const Ink underlinedInk = inkOf(renderer.render(underlined, W, H));
    check(underlinedInk.height() > plainInk.height(), "il sottolineato aggiunge la riga sotto il testo");

    // Sovrapposizione al fotogramma.
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(W) * H * 4, 100);
    for (std::size_t i = 3; i < frame.size(); i += 4)
        frame[i] = 255;
    core::blendTextOver(frame.data(), W * 4, W, H, plain);
    const int cx = plain.x + plainInk.minX;
    int whitePixels = 0;
    for (int y = plain.y; y < plain.y + plain.height; ++y) {
        for (int x = plain.x; x < plain.x + plain.width; ++x) {
            const std::uint8_t* p = &frame[(static_cast<std::size_t>(y) * W + x) * 4];
            if (p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255)
                ++whitePixels;
        }
    }
    check(cx > 0 && whitePixels > 50, "sul fotogramma il testo bianco copre i punti pieni");
    check(frame[0] == 100 && frame[(static_cast<std::size_t>(H) * W - 1) * 4] == 100, "fuori dalla scritta il fotogramma non cambia");
    // Mezza trasparenza: il nero all'alfa 128 su grigio 100 dà circa 50.
    core::TextImage ghost;
    ghost.width = ghost.height = 1;
    ghost.rgba = {0, 0, 0, 128};
    std::vector<std::uint8_t> one{100, 100, 100, 255};
    core::blendTextOver(one.data(), 4, 1, 1, ghost);
    check(one[0] >= 49 && one[0] <= 51 && one[3] == 255, "una scritta semitrasparente si fonde con il fotogramma");
    // Scritta in parte fuori dal fotogramma: nessun errore e ritaglio giusto.
    core::TextImage outside;
    outside.x = -1;
    outside.y = -1;
    outside.width = outside.height = 3;
    outside.rgba.assign(3 * 3 * 4, 255);
    std::vector<std::uint8_t> small(2 * 2 * 4, 0);
    for (std::size_t i = 3; i < small.size(); i += 4)
        small[i] = 255;
    core::blendTextOver(small.data(), 8, 2, 2, outside);
    check(small[0] == 255 && small[12] == 255 && small[15] == 255, "la parte fuori dal fotogramma si scarta");

    std::printf(failures == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", failures);
    return failures == 0 ? 0 : 3;
}

}  // namespace editor::app
