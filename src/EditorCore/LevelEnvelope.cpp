#include "EditorCore/LevelEnvelope.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

std::vector<LevelPoint> normalizeEnvelope(std::vector<LevelPoint> points)
{
    points.erase(std::remove_if(points.begin(), points.end(),
                                [](const LevelPoint& p) { return !std::isfinite(p.time) || !std::isfinite(p.db); }),
                 points.end());
    for (LevelPoint& p : points) {
        p.time = std::max(0.0, p.time);
        p.db = std::clamp(p.db, kLevelMinDb, kLevelMaxDb);
    }
    std::stable_sort(points.begin(), points.end(), [](const LevelPoint& a, const LevelPoint& b) { return a.time < b.time; });
    std::vector<LevelPoint> unique;
    for (const LevelPoint& p : points) {
        if (!unique.empty() && p.time - unique.back().time < 1e-6)
            unique.back() = p;
        else
            unique.push_back(p);
    }
    return unique;
}

double envelopeDbAt(const std::vector<LevelPoint>& points, double seconds)
{
    if (points.empty())
        return 0.0;
    if (seconds <= points.front().time)
        return points.front().db;
    if (seconds >= points.back().time)
        return points.back().db;
    const auto next = std::upper_bound(points.begin(), points.end(), seconds,
                                       [](double t, const LevelPoint& p) { return t < p.time; });
    const LevelPoint& b = *next;
    const LevelPoint& a = *(next - 1);
    const double span = b.time - a.time;
    return span <= 0.0 ? b.db : a.db + (b.db - a.db) * (seconds - a.time) / span;
}

double envelopeGain(double db)
{
    return db <= kLevelMinDb ? 0.0 : std::pow(10.0, db / 20.0);
}

}  // namespace editor::core
