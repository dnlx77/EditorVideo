#include "EditorCore/Animation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

namespace {

std::vector<AnimParam> allParams()
{
    std::vector<AnimParam> params;
    for (std::size_t p = 0; p < kAnimParamCount; ++p)
        params.push_back(static_cast<AnimParam>(p));
    return params;
}

bool moveKeysOf(TransformAnimation& animation, const std::vector<AnimParam>& params, double fromTime, double toTime)
{
    if (!std::isfinite(toTime) || sameTime(fromTime, toTime))
        return false;
    bool any = false;
    for (const AnimParam param : params) {
        const std::vector<AnimKey>& keys = animation.track(param);
        const bool atFrom = std::any_of(keys.begin(), keys.end(), [&](const AnimKey& k) { return sameTime(k.time, fromTime); });
        const bool atTo = std::any_of(keys.begin(), keys.end(), [&](const AnimKey& k) { return sameTime(k.time, toTime); });
        if (atFrom && atTo)
            return false;
        any = any || atFrom;
    }
    if (!any)
        return false;
    for (const AnimParam param : params)
        moveKey(animation, param, fromTime, toTime);
    return true;
}

bool removeKeysOf(TransformAnimation& animation, const std::vector<AnimParam>& params, double time, Transform* base)
{
    bool changed = false;
    for (const AnimParam param : params) {
        const AnimKey* at = findKey(animation, param, time);
        const double value = at != nullptr ? at->value : 0.0;
        if (removeKey(animation, param, time)) {
            changed = true;
            if (base != nullptr && !animation.animated(param))
                setParamValue(*base, param, value);
        }
    }
    return changed;
}

bool setInterpOf(TransformAnimation& animation, const std::vector<AnimParam>& params, double time, Interp interp, double c1x, double c1y,
                 double c2x, double c2y)
{
    bool changed = false;
    for (const AnimParam param : params)
        changed = setKeyInterp(animation, param, time, interp, c1x, c1y, c2x, c2y) || changed;
    return changed;
}

Interp interpOf(const TransformAnimation& animation, const std::vector<AnimParam>& params, double time)
{
    for (const AnimParam param : params) {
        if (const AnimKey* k = findKey(animation, param, time))
            return k->interp;
    }
    return Interp::Linear;
}

}  // namespace

bool moveKeysAt(TransformAnimation& animation, double fromTime, double toTime)
{
    return moveKeysOf(animation, allParams(), fromTime, toTime);
}

void shiftKeys(TransformAnimation& animation, double delta)
{
    for (auto& track : animation.tracks) {
        for (AnimKey& key : track)
            key.time += delta;
    }
}

bool removeKeysAt(TransformAnimation& animation, double time, Transform* base)
{
    return removeKeysOf(animation, allParams(), time, base);
}

bool setKeysInterpAt(TransformAnimation& animation, double time, Interp interp, double c1x, double c1y, double c2x, double c2y)
{
    return setInterpOf(animation, allParams(), time, interp, c1x, c1y, c2x, c2y);
}

Interp interpAt(const TransformAnimation& animation, double time)
{
    return interpOf(animation, allParams(), time);
}

bool moveGroupKeys(TransformAnimation& animation, AnimGroup group, double fromTime, double toTime)
{
    return moveKeysOf(animation, groupParams(group), fromTime, toTime);
}

bool removeGroupKeys(TransformAnimation& animation, AnimGroup group, double time, Transform* base)
{
    return removeKeysOf(animation, groupParams(group), time, base);
}

bool setGroupKeysInterp(TransformAnimation& animation, AnimGroup group, double time, Interp interp, double c1x, double c1y, double c2x,
                        double c2y)
{
    return setInterpOf(animation, groupParams(group), time, interp, c1x, c1y, c2x, c2y);
}

Interp groupInterpAt(const TransformAnimation& animation, AnimGroup group, double time)
{
    return interpOf(animation, groupParams(group), time);
}

const AnimKey* firstGroupKey(const TransformAnimation& animation, AnimGroup group, double time)
{
    for (const AnimParam p : groupParams(group)) {
        if (const AnimKey* k = findKey(animation, p, time))
            return k;
    }
    return nullptr;
}

const std::vector<BezierPreset>& bezierPresets()
{
    static const std::vector<BezierPreset> presets = {
        {"Lineare", 0.0, 0.0, 1.0, 1.0},
        {"Dolce (parte e arriva piano)", 0.42, 0.0, 0.58, 1.0},
        {"Parte piano, poi veloce", 0.42, 0.0, 1.0, 1.0},
        {"Parte veloce, arriva piano", 0.0, 0.0, 0.58, 1.0},
        {"Molto dolce", 0.65, 0.0, 0.35, 1.0},
        {"Rimbalzo in arrivo", 0.34, 1.56, 0.64, 1.0},
    };
    return presets;
}

std::vector<double> groupKeyTimes(const TransformAnimation& animation, AnimGroup group)
{
    std::vector<double> times;
    for (const AnimParam p : groupParams(group)) {
        for (const AnimKey& k : animation.track(p))
            times.push_back(k.time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return sameTime(a, b); }), times.end());
    return times;
}

namespace {

std::string numberText(double value, int decimals)
{
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    std::string out = buffer;
    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0')
            out.pop_back();
        if (!out.empty() && out.back() == '.')
            out.pop_back();
    }
    const std::size_t dot = out.find('.');
    if (dot != std::string::npos)
        out[dot] = ',';
    return out == "-0" ? "0" : out;
}

}  // namespace

std::vector<std::string> describeAnimation(const Transform& base, const TransformAnimation& animation)
{
    std::vector<std::string> lines;
    if (animation.empty())
        return lines;
    const std::vector<double> times = keyTimes(animation);
    const Transform first = evaluateTransform(base, animation, times.front());
    const Transform last = evaluateTransform(base, animation, times.back());
    const auto animated = [&](AnimGroup g) {
        for (const AnimParam p : groupParams(g)) {
            if (animation.animated(p))
                return true;
        }
        return false;
    };
    const auto pair = [&](const std::string& name, const std::string& a, const std::string& b) {
        lines.push_back(name + " " + a + "→" + b);
    };
    if (animated(AnimGroup::Scale))
        pair("Scala", numberText(first.scaleX * 100.0, 1) + "%", numberText(last.scaleX * 100.0, 1) + "%");
    if (animated(AnimGroup::Position))
        pair("Posizione", numberText(first.posX, 0) + ", " + numberText(first.posY, 0) + " px",
             numberText(last.posX, 0) + ", " + numberText(last.posY, 0) + " px");
    if (animated(AnimGroup::Rotation))
        pair("Rotazione", numberText(first.rotation, 1) + "°", numberText(last.rotation, 1) + "°");
    if (animated(AnimGroup::Crop))
        lines.push_back("Ritaglio animato");
    if (animated(AnimGroup::Opacity))
        pair("Opacità", numberText(first.opacity * 100.0, 1) + "%", numberText(last.opacity * 100.0, 1) + "%");
    return lines;
}

const char* animPresetName(AnimPreset preset)
{
    switch (preset) {
        case AnimPreset::ZoomIn: return "Zoom lento avanti";
        case AnimPreset::ZoomOut: return "Zoom lento indietro";
        case AnimPreset::PanLeft: return "Panoramica verso sinistra";
        case AnimPreset::PanRight: return "Panoramica verso destra";
        case AnimPreset::FadeIn: return "Dissolvenza in apertura";
        case AnimPreset::FadeOut: return "Dissolvenza in chiusura";
        case AnimPreset::SlideInLeft: return "Entra da sinistra";
        case AnimPreset::SlideInRight: return "Entra da destra";
        case AnimPreset::SlideInBottom: return "Entra dal basso";
        case AnimPreset::PopIn: return "Compare ingrandendosi";
        case AnimPreset::SlideOutLeft: return "Esce verso sinistra";
        case AnimPreset::SlideOutRight: return "Esce verso destra";
        case AnimPreset::SlideOutBottom: return "Esce verso il basso";
        case AnimPreset::PopOut: return "Sparisce rimpicciolendosi";
        case AnimPreset::Count: break;
    }
    return "";
}

bool applyPreset(TransformAnimation& animation, Transform& base, AnimPreset preset, double from, double to, double projectWidth,
                 double projectHeight)
{
    if (!(to - from > 0.1) || !std::isfinite(from) || !std::isfinite(to))
        return false;
    constexpr double kZoom = 1.15;
    const auto put = [&](AnimParam p, double time, double value, Interp interp) {
        AnimKey key;
        key.time = time;
        key.value = value;
        key.interp = interp;
        setKey(animation, p, key);
    };
    switch (preset) {
        case AnimPreset::ZoomIn:
        case AnimPreset::ZoomOut: {
            const bool in = preset == AnimPreset::ZoomIn;
            for (const AnimParam p : {AnimParam::ScaleX, AnimParam::ScaleY}) {
                const double start = paramValue(base, p);
                clearTrack(animation, p);
                put(p, from, in ? start : start * kZoom, Interp::Smooth);
                put(p, to, in ? start * kZoom : start, Interp::Smooth);
            }
            return true;
        }
        case AnimPreset::PanLeft:
        case AnimPreset::PanRight: {
            clearTrack(animation, AnimParam::ScaleX);
            clearTrack(animation, AnimParam::ScaleY);
            clearTrack(animation, AnimParam::PosX);
            const double zoom = std::max({base.scaleX, base.scaleY, kZoom});
            base.scaleX = base.scaleY = zoom;
            const double reach = std::max(0.0, projectWidth) * (zoom - 1.0) / 2.0 * 0.9;
            const double left = base.posX - reach;
            const double right = base.posX + reach;
            // Panoramica verso sinistra: la "camera" va a sinistra, l'immagine scorre verso destra.
            const bool toLeft = preset == AnimPreset::PanLeft;
            put(AnimParam::PosX, from, toLeft ? left : right, Interp::Smooth);
            put(AnimParam::PosX, to, toLeft ? right : left, Interp::Smooth);
            return true;
        }
        case AnimPreset::FadeIn:
        case AnimPreset::FadeOut: {
            const bool in = preset == AnimPreset::FadeIn;
            const double length = std::min(1.0, (to - from) / 2.0);
            const double full = animation.animated(AnimParam::Opacity) ? evaluateTrack(animation.track(AnimParam::Opacity), in ? from + length : to - length, base.opacity)
                                                                       : base.opacity;
            std::vector<AnimKey>& keys = animation.track(AnimParam::Opacity);
            if (in)
                keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return k.time < from + length; }), keys.end());
            else
                keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return k.time > to - length; }), keys.end());
            if (in) {
                put(AnimParam::Opacity, from, 0.0, Interp::Linear);
                put(AnimParam::Opacity, from + length, full, Interp::Linear);
            } else {
                put(AnimParam::Opacity, to - length, full, Interp::Linear);
                put(AnimParam::Opacity, to, 0.0, Interp::Linear);
            }
            return true;
        }
        case AnimPreset::SlideInLeft:
        case AnimPreset::SlideInRight:
        case AnimPreset::SlideInBottom:
        case AnimPreset::PopIn:
        case AnimPreset::SlideOutLeft:
        case AnimPreset::SlideOutRight:
        case AnimPreset::SlideOutBottom:
        case AnimPreset::PopOut: {
            const bool in = preset == AnimPreset::SlideInLeft || preset == AnimPreset::SlideInRight
                            || preset == AnimPreset::SlideInBottom || preset == AnimPreset::PopIn;
            const double length = std::min(0.8, (to - from) / 2.0);
            // Il tratto del bordo: i valori «nascosti» all'estremo e quelli normali (quelli che il pezzo ha lì, se già animato) all'altro.
            const auto edge = [&](AnimParam p, double (*hidden)(double, double), double d) {
                const double inner = in ? from + length : to - length;
                const double full = animation.animated(p) ? evaluateTrack(animation.track(p), inner, paramValue(base, p)) : paramValue(base, p);
                std::vector<AnimKey>& keys = animation.track(p);
                keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return in ? k.time < inner : k.time > inner; }), keys.end());
                const double outside = hidden(full, d);
                put(p, in ? from : to, outside, Interp::Smooth);
                put(p, inner, full, Interp::Smooth);
            };
            const auto shift = [](double v, double d) { return v + d; };
            const auto zoom = [](double v, double) { return v * 0.6; };
            const double w = std::max(0.0, projectWidth) / 3.0;
            const double h = std::max(0.0, projectHeight) / 3.0;
            switch (preset) {
                case AnimPreset::SlideInLeft: edge(AnimParam::PosX, shift, -w); break;
                case AnimPreset::SlideOutLeft: edge(AnimParam::PosX, shift, -w); break;
                case AnimPreset::SlideInRight: edge(AnimParam::PosX, shift, w); break;
                case AnimPreset::SlideOutRight: edge(AnimParam::PosX, shift, w); break;
                case AnimPreset::SlideInBottom: edge(AnimParam::PosY, shift, h); break;
                case AnimPreset::SlideOutBottom: edge(AnimParam::PosY, shift, h); break;
                default:
                    edge(AnimParam::ScaleX, zoom, 0);
                    edge(AnimParam::ScaleY, zoom, 0);
                    break;
            }
            // Con la dissolvenza insieme: l'opacità sale (o scende) nello stesso tratto.
            const double full = animation.animated(AnimParam::Opacity)
                                    ? evaluateTrack(animation.track(AnimParam::Opacity), in ? from + length : to - length, base.opacity)
                                    : base.opacity;
            std::vector<AnimKey>& keys = animation.track(AnimParam::Opacity);
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const AnimKey& k) { return in ? k.time < from + length : k.time > to - length; }),
                       keys.end());
            if (in) {
                put(AnimParam::Opacity, from, 0.0, Interp::Linear);
                put(AnimParam::Opacity, from + length, full, Interp::Linear);
            } else {
                put(AnimParam::Opacity, to - length, full, Interp::Linear);
                put(AnimParam::Opacity, to, 0.0, Interp::Linear);
            }
            return true;
        }
        case AnimPreset::Count: break;
    }
    return false;
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

std::vector<AnimParam> groupParams(AnimGroup group)
{
    switch (group) {
        case AnimGroup::Scale: return {AnimParam::ScaleX, AnimParam::ScaleY};
        case AnimGroup::Position: return {AnimParam::PosX, AnimParam::PosY};
        case AnimGroup::Rotation: return {AnimParam::Rotation};
        case AnimGroup::Crop: return {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom};
        case AnimGroup::Opacity: return {AnimParam::Opacity};
    }
    return {};
}

namespace {

// La chiave del parametro più vicina a `time` (entro la tolleranza del cursore), nullptr se non c'è.
const AnimKey* keyNear(const std::vector<AnimKey>& keys, double time)
{
    const AnimKey* best = nullptr;
    for (const AnimKey& k : keys) {
        if (std::abs(k.time - time) <= kAnimCursorTolerance && (best == nullptr || std::abs(k.time - time) < std::abs(best->time - time)))
            best = &k;
    }
    return best;
}

}  // namespace

GroupState groupState(const TransformAnimation& animation, AnimGroup group, double time, double from, double to)
{
    GroupState state;
    std::vector<double> times;
    for (const AnimParam p : groupParams(group)) {
        if (!animation.animated(p))
            continue;
        state.animated = true;
        for (const AnimKey& k : animation.track(p))
            times.push_back(k.time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return sameTime(a, b); }), times.end());
    state.keyCount = static_cast<int>(times.size());
    for (const double t : times) {
        if (t < from - kAnimTimeEpsilon || t > to + kAnimTimeEpsilon)
            continue;  // fuori dal pezzo: non si raggiunge
        if (std::abs(t - time) <= kAnimCursorTolerance)
            state.keyAtCursor = true;
        else if (t < time) {
            state.hasPrev = true;
            state.prevTime = t;  // l'ultima prima del cursore (i tempi sono in ordine)
        } else if (!state.hasNext) {
            state.hasNext = true;
            state.nextTime = t;
        }
    }
    return state;
}

bool toggleGroupKey(TransformAnimation& animation, Transform& base, AnimGroup group, double time)
{
    const std::vector<AnimParam> params = groupParams(group);
    const GroupState state = groupState(animation, group, time, -1e18, 1e18);
    if (state.animated && state.keyAtCursor) {
        // Toglie le chiavi sul cursore; un parametro che resta senza chiavi mantiene il valore che aveva lì.
        for (const AnimParam p : params) {
            std::vector<AnimKey>& keys = animation.track(p);
            const AnimKey* at = keyNear(keys, time);
            if (at == nullptr)
                continue;
            const double value = at->value;
            const double atTime = at->time;
            removeKey(animation, p, atTime);
            if (keys.empty())
                setParamValue(base, p, value);
        }
        return true;
    }
    // Aggiunge una chiave per ogni parametro del gruppo, col valore che ha adesso.
    const Transform now = evaluateTransform(base, animation, time);
    for (const AnimParam p : params) {
        AnimKey key;
        key.time = time;
        key.value = paramValue(now, p);
        setKey(animation, p, key);
    }
    return true;
}

void resetGroup(TransformAnimation& animation, Transform& base, AnimGroup group)
{
    const Transform defaults;
    for (const AnimParam p : groupParams(group)) {
        clearTrack(animation, p);
        setParamValue(base, p, paramValue(defaults, p));
    }
}

bool applyEditWithKeys(TransformAnimation& animation, Transform& base, const Transform& shown, const Transform& edited, double time)
{
    bool changed = false;
    Transform newBase = edited;  // parametri non animati (e specchio): valori della modifica
    for (std::size_t i = 0; i < kAnimParamCount; ++i) {
        const AnimParam p = static_cast<AnimParam>(i);
        if (!animation.animated(p))
            continue;
        setParamValue(newBase, p, paramValue(base, p));  // quelli animati restano come sono nella trasformazione fissa
        const double was = paramValue(shown, p);
        const double now = paramValue(edited, p);
        if (std::abs(now - was) <= 1e-12)
            continue;
        AnimKey key;
        key.time = time;
        if (const AnimKey* near = keyNear(animation.track(p), time))
            key = *near;  // si aggiorna la chiave che c'è, con il suo tipo di passaggio
        else
            key.interp = Interp::Linear;
        key.value = now;
        changed = setKey(animation, p, key) || changed;
    }
    newBase = normalized(newBase);
    if (!(newBase == base)) {
        base = newBase;
        changed = true;
    }
    return changed;
}

}  // namespace editor::core
