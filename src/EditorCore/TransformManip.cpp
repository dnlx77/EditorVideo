#include "EditorCore/TransformManip.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool valid(const ManipSize& s)
{
    return s.srcW > 0 && s.srcH > 0 && s.projW > 0 && s.projH > 0;
}

Point2 centerOf(const TransformGeometry& g)
{
    return {(g.corners[0].x + g.corners[1].x + g.corners[2].x + g.corners[3].x) / 4.0,
            (g.corners[0].y + g.corners[1].y + g.corners[2].y + g.corners[3].y) / 4.0};
}

bool insideQuad(const std::array<Point2, 4>& q, Point2 p)
{
    // Dentro se sta dallo stesso lato di tutti i lati (il quadrilatero è un parallelogramma, anche specchiato).
    bool positive = false;
    bool negative = false;
    for (int i = 0; i < 4; ++i) {
        const Point2 a = q[static_cast<std::size_t>(i)];
        const Point2 b = q[static_cast<std::size_t>((i + 1) % 4)];
        const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        positive = positive || cross > 0;
        negative = negative || cross < 0;
    }
    return !(positive && negative);
}

double distance(Point2 a, Point2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

}  // namespace

Point2 manipRotationHandle(const Transform& transform, const ManipSize& size, double pxPerUnit)
{
    const TransformGeometry g = computeGeometry(normalized(transform), size.srcW, size.srcH, size.projW, size.projH);
    const Point2 mid{(g.corners[0].x + g.corners[1].x) / 2.0, (g.corners[0].y + g.corners[1].y) / 2.0};
    const Point2 center = centerOf(g);
    double dx = mid.x - center.x;
    double dy = mid.y - center.y;
    const double length = std::hypot(dx, dy);
    if (length < 1e-9) {
        dx = 0.0;
        dy = -1.0;
    } else {
        dx /= length;
        dy /= length;
    }
    const double offset = kManipRotateDistance / std::max(pxPerUnit, 1e-9);
    return {mid.x + dx * offset, mid.y + dy * offset};
}

ManipHit manipHitTest(const Transform& transform, const ManipSize& size, Point2 at, double pxPerUnit)
{
    if (!valid(size))
        return {};
    const TransformGeometry g = computeGeometry(normalized(transform), size.srcW, size.srcH, size.projW, size.projH);
    const double radius = kManipHandleRadius / std::max(pxPerUnit, 1e-9);
    if (distance(at, manipRotationHandle(transform, size, pxPerUnit)) <= radius)
        return {ManipPart::Rotate, -1};
    int best = -1;
    double bestDistance = radius;
    for (int i = 0; i < 4; ++i) {
        const double d = distance(at, g.corners[static_cast<std::size_t>(i)]);
        if (d <= bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    if (best >= 0)
        return {ManipPart::Corner, best};
    if (insideQuad(g.corners, at))
        return {ManipPart::Move, -1};
    return {};
}

Transform manipMove(const Transform& start, Point2 from, Point2 to)
{
    Transform t = start;
    t.posX += to.x - from.x;
    t.posY += to.y - from.y;
    return normalized(t);
}

Transform manipScale(const Transform& start, const ManipSize& size, int corner, Point2 to, bool freeAspect, bool fromCenter)
{
    if (!valid(size) || corner < 0 || corner > 3)
        return start;
    const Transform t0 = normalized(start);
    const TransformGeometry g = computeGeometry(t0, size.srcW, size.srcH, size.projW, size.projH);
    const std::array<Point2, 4> source = {Point2{g.sourceLeft, g.sourceTop}, Point2{g.sourceRight, g.sourceTop},
                                          Point2{g.sourceRight, g.sourceBottom}, Point2{g.sourceLeft, g.sourceBottom}};
    const Point2 dragged = source[static_cast<std::size_t>(corner)];
    Point2 anchorSource = source[static_cast<std::size_t>((corner + 2) % 4)];
    Point2 anchor = g.corners[static_cast<std::size_t>((corner + 2) % 4)];
    if (fromCenter) {
        anchorSource = {(g.sourceLeft + g.sourceRight) / 2.0, (g.sourceTop + g.sourceBottom) / 2.0};
        anchor = centerOf(g);
    }

    const double fit = std::min(size.projW / size.srcW, size.projH / size.srcH);
    const double fx = t0.flipHorizontal ? -1.0 : 1.0;
    const double fy = t0.flipVertical ? -1.0 : 1.0;
    const double radians = t0.rotation * kPi / 180.0;
    const double cs = std::cos(radians);
    const double sn = std::sin(radians);
    // Il mouse rispetto all'ancora, nel sistema ruotato del pezzo.
    const double mx = to.x - anchor.x;
    const double my = to.y - anchor.y;
    const double wx = cs * mx + sn * my;
    const double wy = -sn * mx + cs * my;
    const double ex = dragged.x - anchorSource.x;
    const double ey = dragged.y - anchorSource.y;

    Transform t = t0;
    if (freeAspect) {
        if (std::abs(ex) > 1e-9)
            t.scaleX = wx / (fit * fx * ex);
        if (std::abs(ey) > 1e-9)
            t.scaleY = wy / (fit * fy * ey);
    } else {
        const double vx = fit * fx * t0.scaleX * ex;
        const double vy = fit * fy * t0.scaleY * ey;
        const double norm = vx * vx + vy * vy;
        const double k = norm > 1e-12 ? (wx * vx + wy * vy) / norm : 1.0;
        t.scaleX = t0.scaleX * k;
        t.scaleY = t0.scaleY * k;
    }
    t.scaleX = std::clamp(t.scaleX, kMinScale, kMaxScale);
    t.scaleY = std::clamp(t.scaleY, kMinScale, kMaxScale);

    // La nuova posizione: il punto d'ancora resta dov'è.
    const double fileCx = size.srcW / 2.0;
    const double fileCy = size.srcH / 2.0;
    const double dx = fit * fx * t.scaleX * (anchorSource.x - fileCx);
    const double dy = fit * fy * t.scaleY * (anchorSource.y - fileCy);
    const double centerX = anchor.x - (cs * dx - sn * dy);
    const double centerY = anchor.y - (sn * dx + cs * dy);
    t.posX = centerX - size.projW / 2.0;
    t.posY = centerY - size.projH / 2.0;
    return normalized(t);
}

Transform manipRotate(const Transform& start, const ManipSize& size, Point2 from, Point2 to, bool snap)
{
    if (!valid(size))
        return start;
    const Transform t0 = normalized(start);
    const Point2 pivot{size.projW / 2.0 + t0.posX, size.projH / 2.0 + t0.posY};
    const double a0 = std::atan2(from.y - pivot.y, from.x - pivot.x);
    const double a1 = std::atan2(to.y - pivot.y, to.x - pivot.x);
    double delta = (a1 - a0) * 180.0 / kPi;
    while (delta > 180.0)
        delta -= 360.0;
    while (delta < -180.0)
        delta += 360.0;
    Transform t = t0;
    t.rotation = t0.rotation + delta;
    if (snap)
        t.rotation = std::round(t.rotation / 15.0) * 15.0;
    return normalized(t);
}

}  // namespace editor::core
