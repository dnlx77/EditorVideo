#include "EditorCore/Animation.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

namespace {

bool sameTime(double a, double b)
{
    return std::abs(a - b) < kAnimTimeEpsilon;
}

// Bézier cubica con estremi (0,0) e (1,1): x(s) e y(s) per s in 0..1.
double bez(double p1, double p2, double s)
{
    const double r = 1.0 - s;
    return 3.0 * r * r * s * p1 + 3.0 * r * s * s * p2 + s * s * s;
}

// Il valore y della curva quando x vale `u`: si cerca s con x(s) = u (x è crescente perché c1x, c2x sono in 0..1).
double bezierAt(double c1x, double c1y, double c2x, double c2y, double u)
{
    if (u <= 0.0)
        return 0.0;
    if (u >= 1.0)
        return 1.0;
    double lo = 0.0, hi = 1.0, s = u;
    for (int i = 0; i < 40; ++i) {
        const double x = bez(c1x, c2x, s);
        if (std::abs(x - u) < 1e-10)
            break;
        if (x < u)
            lo = s;
        else
            hi = s;
        s = 0.5 * (lo + hi);
    }
    return bez(c1y, c2y, s);
}

}  // namespace

bool TransformAnimation::empty() const
{
    return std::all_of(tracks.begin(), tracks.end(), [](const std::vector<AnimKey>& t) { return t.empty(); });
}

double paramValue(const Transform& t, AnimParam p)
{
    switch (p) {
        case AnimParam::ScaleX: return t.scaleX;
        case AnimParam::ScaleY: return t.scaleY;
        case AnimParam::PosX: return t.posX;
        case AnimParam::PosY: return t.posY;
        case AnimParam::Rotation: return t.rotation;
        case AnimParam::CropLeft: return t.cropLeft;
        case AnimParam::CropRight: return t.cropRight;
        case AnimParam::CropTop: return t.cropTop;
        case AnimParam::CropBottom: return t.cropBottom;
        case AnimParam::Opacity: return t.opacity;
        case AnimParam::Count: break;
    }
    return 0.0;
}

void setParamValue(Transform& t, AnimParam p, double value)
{
    switch (p) {
        case AnimParam::ScaleX: t.scaleX = value; break;
        case AnimParam::ScaleY: t.scaleY = value; break;
        case AnimParam::PosX: t.posX = value; break;
        case AnimParam::PosY: t.posY = value; break;
        case AnimParam::Rotation: t.rotation = value; break;
        case AnimParam::CropLeft: t.cropLeft = value; break;
        case AnimParam::CropRight: t.cropRight = value; break;
        case AnimParam::CropTop: t.cropTop = value; break;
        case AnimParam::CropBottom: t.cropBottom = value; break;
        case AnimParam::Opacity: t.opacity = value; break;
        case AnimParam::Count: break;
    }
}

const char* animParamName(AnimParam p)
{
    switch (p) {
        case AnimParam::ScaleX: return "Scala X";
        case AnimParam::ScaleY: return "Scala Y";
        case AnimParam::PosX: return "Posizione X";
        case AnimParam::PosY: return "Posizione Y";
        case AnimParam::Rotation: return "Rotazione";
        case AnimParam::CropLeft: return "Ritaglio sinistra";
        case AnimParam::CropRight: return "Ritaglio destra";
        case AnimParam::CropTop: return "Ritaglio alto";
        case AnimParam::CropBottom: return "Ritaglio basso";
        case AnimParam::Opacity: return "Opacità";
        case AnimParam::Count: break;
    }
    return "";
}

double easedFraction(const AnimKey& from, double u)
{
    u = std::clamp(u, 0.0, 1.0);
    switch (from.interp) {
        case Interp::Linear: return u;
        case Interp::Hold: return 0.0;
        case Interp::Smooth: return bezierAt(0.42, 0.0, 0.58, 1.0, u);
        case Interp::Bezier: return bezierAt(std::clamp(from.c1x, 0.0, 1.0), from.c1y, std::clamp(from.c2x, 0.0, 1.0), from.c2y, u);
    }
    return u;
}

double evaluateTrack(const std::vector<AnimKey>& keys, double time, double fallback)
{
    if (keys.empty())
        return fallback;
    if (!std::isfinite(time) || time <= keys.front().time)
        return keys.front().value;
    if (time >= keys.back().time)
        return keys.back().value;
    // Ultima chiave con tempo <= time (ce n'è una successiva perché time < back.time).
    const auto next = std::upper_bound(keys.begin(), keys.end(), time, [](double t, const AnimKey& k) { return t < k.time; });
    const AnimKey& a = *(next - 1);
    const AnimKey& b = *next;
    const double span = b.time - a.time;
    if (span <= 0.0)
        return b.value;
    return a.value + (b.value - a.value) * easedFraction(a, (time - a.time) / span);
}

Transform evaluateTransform(const Transform& base, const TransformAnimation& animation, double time)
{
    Transform t = base;
    for (std::size_t i = 0; i < kAnimParamCount; ++i) {
        const AnimParam p = static_cast<AnimParam>(i);
        if (animation.animated(p))
            setParamValue(t, p, evaluateTrack(animation.track(p), time, paramValue(base, p)));
    }
    return normalized(t);
}

void normalizeAnimation(TransformAnimation& animation)
{
    for (std::vector<AnimKey>& keys : animation.tracks) {
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [](const AnimKey& k) { return !std::isfinite(k.time) || !std::isfinite(k.value); }),
                   keys.end());
        for (AnimKey& k : keys) {
            const AnimKey def;
            if (!std::isfinite(k.c1x) || !std::isfinite(k.c1y) || !std::isfinite(k.c2x) || !std::isfinite(k.c2y)) {
                k.c1x = def.c1x;
                k.c1y = def.c1y;
                k.c2x = def.c2x;
                k.c2y = def.c2y;
            }
            k.c1x = std::clamp(k.c1x, 0.0, 1.0);
            k.c2x = std::clamp(k.c2x, 0.0, 1.0);
            k.c1y = std::clamp(k.c1y, -10.0, 10.0);
            k.c2y = std::clamp(k.c2y, -10.0, 10.0);
        }
        std::stable_sort(keys.begin(), keys.end(), [](const AnimKey& a, const AnimKey& b) { return a.time < b.time; });
        // Tempi doppi: vince l'ultima delle due.
        std::vector<AnimKey> unique;
        for (const AnimKey& k : keys) {
            if (!unique.empty() && sameTime(unique.back().time, k.time))
                unique.back() = k;
            else
                unique.push_back(k);
        }
        keys = std::move(unique);
    }
}

bool setKey(TransformAnimation& animation, AnimParam param, AnimKey key)
{
    if (!std::isfinite(key.time) || !std::isfinite(key.value))
        return false;
    std::vector<AnimKey>& keys = animation.track(param);
    for (AnimKey& k : keys) {
        if (!sameTime(k.time, key.time))
            continue;
        key.time = k.time;
        if (k == key)
            return false;
        k = key;
        return true;
    }
    keys.push_back(key);
    TransformAnimation one;
    one.track(param) = std::move(keys);
    normalizeAnimation(one);
    keys = std::move(one.track(param));
    return true;
}

bool removeKey(TransformAnimation& animation, AnimParam param, double time)
{
    std::vector<AnimKey>& keys = animation.track(param);
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return sameTime(k.time, time); });
    if (it == keys.end())
        return false;
    keys.erase(it);
    return true;
}

bool moveKey(TransformAnimation& animation, AnimParam param, double fromTime, double toTime)
{
    if (!std::isfinite(toTime))
        return false;
    std::vector<AnimKey>& keys = animation.track(param);
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return sameTime(k.time, fromTime); });
    if (it == keys.end())
        return false;
    if (sameTime(fromTime, toTime))
        return false;
    for (const AnimKey& k : keys) {
        if (&k != &*it && sameTime(k.time, toTime))
            return false;
    }
    it->time = toTime;
    std::stable_sort(keys.begin(), keys.end(), [](const AnimKey& a, const AnimKey& b) { return a.time < b.time; });
    return true;
}

bool setKeyInterp(TransformAnimation& animation, AnimParam param, double time, Interp interp, double c1x, double c1y, double c2x,
                  double c2y)
{
    std::vector<AnimKey>& keys = animation.track(param);
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return sameTime(k.time, time); });
    if (it == keys.end())
        return false;
    AnimKey changed = *it;
    changed.interp = interp;
    if (interp == Interp::Bezier) {
        changed.c1x = c1x;
        changed.c1y = c1y;
        changed.c2x = c2x;
        changed.c2y = c2y;
        TransformAnimation one;
        one.track(param) = {changed};
        normalizeAnimation(one);
        if (one.track(param).empty())
            return false;
        changed = one.track(param).front();
    }
    if (changed == *it)
        return false;
    *it = changed;
    return true;
}

bool clearTrack(TransformAnimation& animation, AnimParam param)
{
    if (animation.track(param).empty())
        return false;
    animation.track(param).clear();
    return true;
}

const AnimKey* findKey(const TransformAnimation& animation, AnimParam param, double time)
{
    for (const AnimKey& k : animation.track(param)) {
        if (sameTime(k.time, time))
            return &k;
    }
    return nullptr;
}

std::vector<double> keyTimes(const TransformAnimation& animation)
{
    std::vector<double> times;
    for (const std::vector<AnimKey>& keys : animation.tracks) {
        for (const AnimKey& k : keys)
            times.push_back(k.time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return sameTime(a, b); }), times.end());
    return times;
}

}  // namespace editor::core
