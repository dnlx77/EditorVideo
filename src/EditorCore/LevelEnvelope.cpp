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

std::vector<LevelPoint> envelopeForTrim(std::vector<LevelPoint> points, bool atStart, double oldEdge, double newEdge, double limit)
{
    constexpr double kReach = 0.05;  // quanto può stare lontano dal bordo l'ultimo punto per dirlo attaccato
    constexpr double kStep = 1e-3;
    constexpr double kSameDb = 1e-6;
    const double shift = newEdge - oldEdge;
    if (points.size() < 1 || std::abs(shift) < kStep)
        return points;
    const int n = static_cast<int>(points.size());
    if (!atStart) {
        // Finisce col bordo? Allora la sfumatura finale (i punti con livelli diversi, dall'ultimo all'indietro) lo segue.
        if (points.back().time < oldEdge - kReach)
            return points;
        int first = n - 1;
        while (first > 0 && std::abs(points[first - 1].db - points[first].db) > kSameDb)
            --first;
        for (int i = first; i < n; ++i) {
            const double floor = i > 0 ? points[i - 1].time + kStep : limit;
            points[i].time = std::max(points[i].time + shift, floor);
        }
    } else {
        if (points.front().time > oldEdge + kReach)
            return points;
        int last = 0;
        while (last + 1 < n && std::abs(points[last + 1].db - points[last].db) > kSameDb)
            ++last;
        for (int i = last; i >= 0; --i) {
            const double ceiling = i + 1 < n ? points[i + 1].time - kStep : limit;
            points[i].time = std::min(points[i].time + shift, ceiling);
        }
    }
    return normalizeEnvelope(std::move(points));
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
