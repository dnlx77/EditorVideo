#include "EditorCore/Transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace editor::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

// Un lato opposto all'altro: si riducono in proporzione se la somma supera il limite.
void limitPair(double& a, double& b)
{
    a = std::clamp(finiteOr(a, 0.0), 0.0, kMaxCrop);
    b = std::clamp(finiteOr(b, 0.0), 0.0, kMaxCrop);
    const double sum = a + b;
    if (sum > kMaxCrop) {
        a *= kMaxCrop / sum;
        b *= kMaxCrop / sum;
    }
}

}  // namespace

bool Transform::movesPicture() const
{
    Transform base;
    base.opacity = opacity;
    return !(*this == base);
}

Transform normalized(Transform t)
{
    t.scaleX = std::clamp(finiteOr(t.scaleX, 1.0), kMinScale, kMaxScale);
    t.scaleY = std::clamp(finiteOr(t.scaleY, 1.0), kMinScale, kMaxScale);
    t.posX = std::clamp(finiteOr(t.posX, 0.0), -kMaxPosition, kMaxPosition);
    t.posY = std::clamp(finiteOr(t.posY, 0.0), -kMaxPosition, kMaxPosition);
    t.rotation = std::clamp(finiteOr(t.rotation, 0.0), -kMaxRotation, kMaxRotation);
    limitPair(t.cropLeft, t.cropRight);
    limitPair(t.cropTop, t.cropBottom);
    t.opacity = std::clamp(finiteOr(t.opacity, 1.0), 0.0, 1.0);
    return t;
}

Affine Affine::inverse() const
{
    const double det = a * d - b * c;
    if (std::abs(det) < 1e-12)
        return {};
    Affine r;
    r.a = d / det;
    r.b = -b / det;
    r.c = -c / det;
    r.d = a / det;
    r.tx = -(r.a * tx + r.c * ty);
    r.ty = -(r.b * tx + r.d * ty);
    return r;
}

TransformGeometry computeGeometry(const Transform& transform, int sourceWidth, int sourceHeight, int projectWidth, int projectHeight)
{
    TransformGeometry g;
    if (sourceWidth <= 0 || sourceHeight <= 0 || projectWidth <= 0 || projectHeight <= 0)
        return g;
    const Transform t = normalized(transform);
    const double sw = sourceWidth;
    const double sh = sourceHeight;

    // Adattato al progetto (scala 100%), poi la scala scelta, con lo specchio come scala negativa.
    const double fit = std::min(projectWidth / sw, projectHeight / sh);
    const double ex = fit * t.scaleX * (t.flipHorizontal ? -1.0 : 1.0);
    const double ey = fit * t.scaleY * (t.flipVertical ? -1.0 : 1.0);
    const double radians = t.rotation * kPi / 180.0;
    const double cs = std::cos(radians);
    const double sn = std::sin(radians);
    // p' = centro + R * D * (p - centro del file); con y verso il basso, un angolo positivo gira in senso orario.
    g.toProject.a = cs * ex;
    g.toProject.b = sn * ex;
    g.toProject.c = -sn * ey;
    g.toProject.d = cs * ey;
    const double cx = projectWidth / 2.0 + t.posX;
    const double cy = projectHeight / 2.0 + t.posY;
    g.toProject.tx = cx - (g.toProject.a * sw / 2.0 + g.toProject.c * sh / 2.0);
    g.toProject.ty = cy - (g.toProject.b * sw / 2.0 + g.toProject.d * sh / 2.0);

    // I ritagli sono quelli che si vedono sullo schermo: con lo specchio il lato sinistro dello schermo è quello destro del file.
    g.sourceLeft = (t.flipHorizontal ? t.cropRight : t.cropLeft) * sw;
    g.sourceRight = (1.0 - (t.flipHorizontal ? t.cropLeft : t.cropRight)) * sw;
    g.sourceTop = (t.flipVertical ? t.cropBottom : t.cropTop) * sh;
    g.sourceBottom = (1.0 - (t.flipVertical ? t.cropTop : t.cropBottom)) * sh;
    g.corners = {g.toProject.apply(g.sourceLeft, g.sourceTop), g.toProject.apply(g.sourceRight, g.sourceTop),
                 g.toProject.apply(g.sourceRight, g.sourceBottom), g.toProject.apply(g.sourceLeft, g.sourceBottom)};
    g.fullCorners = {g.toProject.apply(0, 0), g.toProject.apply(sw, 0), g.toProject.apply(sw, sh), g.toProject.apply(0, sh)};
    g.opacity = t.opacity;
    return g;
}

namespace {

// Un numero senza zeri inutili: 150, 12,5 (virgola all'italiana).
std::string number(double value, int decimals)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", decimals, value);
    std::string out = text;
    if (out.find('.') != std::string::npos) {
        while (out.back() == '0')
            out.pop_back();
        if (out.back() == '.')
            out.pop_back();
    }
    const std::size_t dot = out.find('.');
    if (dot != std::string::npos)
        out[dot] = ',';
    return out == "-0" ? "0" : out;
}

}  // namespace

std::vector<std::string> describeTransform(const Transform& input)
{
    const Transform t = normalized(input);
    const Transform base;
    std::vector<std::string> lines;
    if (t.scaleX != base.scaleX || t.scaleY != base.scaleY) {
        if (std::abs(t.scaleX - t.scaleY) < 1e-9)
            lines.push_back("Scala " + number(t.scaleX * 100.0, 1) + "%");
        else
            lines.push_back("Scala " + number(t.scaleX * 100.0, 1) + "% × " + number(t.scaleY * 100.0, 1) + "%");
    }
    if (t.posX != base.posX || t.posY != base.posY)
        lines.push_back("Posizione " + number(t.posX, 1) + ", " + number(t.posY, 1) + " px");
    if (t.rotation != base.rotation)
        lines.push_back("Rotazione " + number(t.rotation, 1) + "°");
    if (t.cropLeft != 0.0 || t.cropRight != 0.0 || t.cropTop != 0.0 || t.cropBottom != 0.0) {
        std::string crop = "Ritaglio";
        const auto side = [&](const char* name, double value) {
            if (value != 0.0)
                crop += std::string(" ") + name + " " + number(value * 100.0, 1) + "%";
        };
        side("sinistra", t.cropLeft);
        side("destra", t.cropRight);
        side("alto", t.cropTop);
        side("basso", t.cropBottom);
        lines.push_back(crop);
    }
    if (t.flipHorizontal)
        lines.push_back("Specchiato in orizzontale");
    if (t.flipVertical)
        lines.push_back("Specchiato in verticale");
    if (t.opacity != base.opacity)
        lines.push_back("Opacità " + number(t.opacity * 100.0, 1) + "%");
    return lines;
}

}  // namespace editor::core
