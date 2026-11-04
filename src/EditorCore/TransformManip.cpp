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

// ---- Ritaglio

namespace {

// Un punto dell'immagine intera, in frazioni di come la si vede (0..1 da sinistra e dall'alto dello schermo), sul progetto.
Point2 displayedToProject(const Transform& t, const TransformGeometry& g, const ManipSize& size, double fx, double fy)
{
    const double u = (t.flipHorizontal ? 1.0 - fx : fx) * size.srcW;
    const double v = (t.flipVertical ? 1.0 - fy : fy) * size.srcH;
    return g.toProject.apply(u, v);
}

double distanceToSegment(Point2 p, Point2 a, Point2 b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length2 = dx * dx + dy * dy;
    const double along = length2 < 1e-12 ? 0.0 : std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.0, 1.0);
    return distance(p, {a.x + along * dx, a.y + along * dy});
}

}  // namespace

Point2 cropHandlePoint(const Transform& transform, const ManipSize& size, int edges)
{
    const Transform t = normalized(transform);
    const TransformGeometry g = computeGeometry(t, static_cast<int>(size.srcW), static_cast<int>(size.srcH),
                                                static_cast<int>(size.projW), static_cast<int>(size.projH));
    const double left = t.cropLeft;
    const double right = 1.0 - t.cropRight;
    const double top = t.cropTop;
    const double bottom = 1.0 - t.cropBottom;
    const double fx = (edges & kCropLeft) ? left : (edges & kCropRight) ? right : (left + right) / 2.0;
    const double fy = (edges & kCropTop) ? top : (edges & kCropBottom) ? bottom : (top + bottom) / 2.0;
    return displayedToProject(t, g, size, fx, fy);
}

int cropHitTest(const Transform& transform, const ManipSize& size, Point2 at, double pxPerUnit)
{
    if (!valid(size))
        return 0;
    const double radius = kManipHandleRadius / std::max(pxPerUnit, 1e-9);
    constexpr int corners[4] = {kCropLeft | kCropTop, kCropRight | kCropTop, kCropRight | kCropBottom, kCropLeft | kCropBottom};
    int best = 0;
    double bestDistance = radius;
    for (const int edges : corners) {
        const double d = distance(at, cropHandlePoint(transform, size, edges));
        if (d <= bestDistance) {
            best = edges;
            bestDistance = d;
        }
    }
    if (best != 0)
        return best;
    struct Side
    {
        int edge;
        int from;
        int to;
    };
    constexpr Side sides[4] = {{kCropTop, corners[0], corners[1]},
                               {kCropRight, corners[1], corners[2]},
                               {kCropBottom, corners[3], corners[2]},
                               {kCropLeft, corners[0], corners[3]}};
    bestDistance = radius * 0.75;
    for (const Side& side : sides) {
        const double d = distanceToSegment(at, cropHandlePoint(transform, size, side.from), cropHandlePoint(transform, size, side.to));
        if (d <= bestDistance) {
            best = side.edge;
            bestDistance = d;
        }
    }
    return best;
}

Transform manipCrop(const Transform& start, const ManipSize& size, int edges, Point2 to)
{
    if (!valid(size) || edges == 0)
        return start;
    Transform t = normalized(start);
    const TransformGeometry g = computeGeometry(t, static_cast<int>(size.srcW), static_cast<int>(size.srcH),
                                                static_cast<int>(size.projW), static_cast<int>(size.projH));
    const Point2 inFile = g.toProject.inverse().apply(to.x, to.y);
    double fx = inFile.x / size.srcW;
    double fy = inFile.y / size.srcH;
    if (t.flipHorizontal)
        fx = 1.0 - fx;
    if (t.flipVertical)
        fy = 1.0 - fy;
    if (edges & kCropLeft)
        t.cropLeft = std::clamp(fx, 0.0, 1.0 - t.cropRight - kCropMinVisible);
    if (edges & kCropRight)
        t.cropRight = std::clamp(1.0 - fx, 0.0, 1.0 - t.cropLeft - kCropMinVisible);
    if (edges & kCropTop)
        t.cropTop = std::clamp(fy, 0.0, 1.0 - t.cropBottom - kCropMinVisible);
    if (edges & kCropBottom)
        t.cropBottom = std::clamp(1.0 - fy, 0.0, 1.0 - t.cropTop - kCropMinVisible);
    return normalized(t);
}

// ---- Guide magnetiche

Transform snapMove(const Transform& moved, const ManipSize& size, double pxPerUnit, SnapGuides* guides)
{
    if (guides != nullptr)
        *guides = {};
    if (!valid(size))
        return moved;
    Transform t = normalized(moved);
    const TransformGeometry g = computeGeometry(t, static_cast<int>(size.srcW), static_cast<int>(size.srcH),
                                                static_cast<int>(size.projW), static_cast<int>(size.projH));
    double minX = g.corners[0].x, maxX = minX, minY = g.corners[0].y, maxY = minY;
    for (const Point2& p : g.corners) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const Point2 center = centerOf(g);
    const double threshold = kSnapDistance / std::max(pxPerUnit, 1e-9);
    // Per ogni asse: la correzione più piccola fra (bordi e centro del pezzo) e (bordi e centro del fotogramma).
    const auto snap = [&](const double features[3], const double targets[3], double& correction, double& target) {
        double best = threshold + 1.0;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                const double delta = targets[j] - features[i];
                if (std::abs(delta) <= threshold && std::abs(delta) < std::abs(best)) {
                    best = delta;
                    target = targets[j];
                }
            }
        }
        correction = std::abs(best) <= threshold ? best : 0.0;
        return std::abs(best) <= threshold;
    };
    const double xs[3] = {minX, center.x, maxX};
    const double ys[3] = {minY, center.y, maxY};
    const double targetsX[3] = {0.0, size.projW / 2.0, size.projW};
    const double targetsY[3] = {0.0, size.projH / 2.0, size.projH};
    double dx = 0.0, dy = 0.0, lineX = 0.0, lineY = 0.0;
    const bool snappedX = snap(xs, targetsX, dx, lineX);
    const bool snappedY = snap(ys, targetsY, dy, lineY);
    t.posX += dx;
    t.posY += dy;
    if (guides != nullptr) {
        if (snappedX)
            guides->vertical.push_back(lineX);
        if (snappedY)
            guides->horizontal.push_back(lineY);
    }
    return normalized(t);
}

Transform snapRotation(const Transform& rotated, bool* snapped)
{
    Transform t = rotated;
    const double nearest = std::round(t.rotation / 90.0) * 90.0;
    const bool near = std::abs(t.rotation - nearest) <= kSnapDegrees;
    if (near)
        t.rotation = nearest;
    if (snapped != nullptr)
        *snapped = near;
    return t;
}

Transform snapScale(const Transform& scaled)
{
    Transform t = scaled;
    if (std::abs(t.scaleX - 1.0) <= kSnapScale)
        t.scaleX = 1.0;
    if (std::abs(t.scaleY - 1.0) <= kSnapScale)
        t.scaleY = 1.0;
    return t;
}

}  // namespace editor::core
