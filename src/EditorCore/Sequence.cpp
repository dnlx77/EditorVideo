#include "EditorCore/Sequence.h"

#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace editor::core {

namespace {

// Aggiunge [from, to) a un elenco di intervalli in ordine, unendo quelli che si toccano.
void addRange(std::vector<TimeRange>& ranges, double from, double to)
{
    if (to <= from + kTimeEpsilon)
        return;
    std::vector<TimeRange> result;
    TimeRange added{from, to};
    for (const TimeRange& r : ranges) {
        if (r.end < added.start - kTimeEpsilon || r.start > added.end + kTimeEpsilon) {
            result.push_back(r);
            continue;
        }
        added = {std::min(added.start, r.start), std::max(added.end, r.end)};
    }
    result.push_back(added);
    std::sort(result.begin(), result.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    ranges = std::move(result);
}

// Toglie [from, to) da un elenco di intervalli.
void subtractRange(std::vector<TimeRange>& ranges, double from, double to)
{
    std::vector<TimeRange> result;
    for (const TimeRange& r : ranges) {
        if (r.end <= from + kTimeEpsilon || r.start >= to - kTimeEpsilon) {
            result.push_back(r);
            continue;
        }
        if (r.start < from - kTimeEpsilon)
            result.push_back({r.start, from});
        if (r.end > to + kTimeEpsilon)
            result.push_back({to, r.end});
    }
    ranges = std::move(result);
}

// Solo la parte degli intervalli dentro [from, to).
std::vector<TimeRange> clipRanges(const std::vector<TimeRange>& ranges, double from, double to)
{
    std::vector<TimeRange> result;
    for (const TimeRange& r : ranges) {
        const double start = std::max(r.start, from);
        const double end = std::min(r.end, to);
        if (end > start + kTimeEpsilon)
            result.push_back({start, end});
    }
    return result;
}

}  // namespace

int Sequence::insert(int index, int clipId, double start, double end)
{
    start = std::max(0.0, start);
    if (end <= start + kTimeEpsilon)
        return 0;
    Fragment fragment{nextId_++, clipId, start, end};
    const int count = static_cast<int>(fragments_.size());
    const int at = index < 0 || index > count ? count : index;
    // Subito dopo il precedente: lo spazio vuoto che c'era lì (prima del pezzo che ora viene dopo) si riempie.
    if (at < count)
        fragments_[at].gap = std::max(0.0, fragments_[at].gap - fragment.length());
    fragments_.insert(fragments_.begin() + at, fragment);
    return fragment.id;
}

bool Sequence::remove(int fragmentId)
{
    const int index = indexOf(fragmentId);
    if (index < 0)
        return false;
    const double gap = fragments_[index].gap;
    fragments_.erase(fragments_.begin() + index);
    if (index < static_cast<int>(fragments_.size()))
        fragments_[index].gap += gap;  // lo spazio prima del pezzo tolto resta
    return true;
}

void Sequence::removeClip(int clipId)
{
    for (int i = static_cast<int>(fragments_.size()) - 1; i >= 0; --i) {
        if (fragments_[i].clipId == clipId)
            remove(fragments_[i].id);
    }
}

void Sequence::move(int fragmentId, int toIndex)
{
    const int from = indexOf(fragmentId);
    if (from < 0)
        return;
    toIndex = std::clamp(toIndex, 0, static_cast<int>(fragments_.size()) - 1);
    Fragment fragment = fragments_[from];
    remove(fragmentId);
    fragment.gap = 0.0;
    fragments_.insert(fragments_.begin() + toIndex, fragment);
}

double Sequence::nearestFreeStart(int fragmentId, double desired, bool push) const
{
    const int self = indexOf(fragmentId);
    if (self < 0)
        return -1.0;
    const double length = fragments_[self].length();
    double oldStart = 0.0;
    double previousEnd = 0.0;  // fine del pezzo che sta prima di lui
    double t = 0.0;
    for (int i = 0; i <= self; ++i) {
        t += fragments_[static_cast<std::size_t>(i)].gap;
        if (i == self)
            oldStart = t;
        else
            previousEnd = t + fragments_[static_cast<std::size_t>(i)].length();
        t += fragments_[static_cast<std::size_t>(i)].length();
    }
    // Verso destra il pezzo spinge quelli dopo di lui: si può andare dovunque, non prima della fine del precedente.
    if (push && desired > oldStart + kTimeEpsilon)
        return std::max(desired, previousEnd);
    double best = -1.0;
    double bestDistance = 0.0;
    // Gli spazi liberi fra gli altri pezzi (che restano dove sono): in ognuno il punto più vicino a `desired`.
    const auto consider = [&](double from, double to) {
        if (to - from < length - kTimeEpsilon)
            return;  // lì non ci sta
        const double candidate = std::max(from, std::min(desired, to - length));
        const double distance = std::abs(candidate - desired);
        if (best < 0 || distance < bestDistance - kTimeEpsilon) {
            best = candidate;
            bestDistance = distance;
        }
    };
    double freeFrom = 0.0;
    t = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        t += fragments_[i].gap;
        if (static_cast<int>(i) != self) {
            consider(freeFrom, t);
            freeFrom = t + fragments_[i].length();
        }
        t += fragments_[i].length();
    }
    consider(freeFrom, std::numeric_limits<double>::infinity());
    return best;
}

double Sequence::trimLimit(int fragmentId, Edge edge, double desired, double sourceDuration, double minLength,
                           bool ripple) const
{
    const int self = indexOf(fragmentId);
    if (self < 0)
        return -1.0;
    const Fragment& f = fragments_[static_cast<std::size_t>(self)];
    minLength = std::max(minLength, kTimeEpsilon);
    if (edge == Edge::Start) {
        // Senza ripple l'inizio nella sequenza si sposta: non prima della fine del pezzo precedente.
        const double lowest = std::max(0.0, ripple ? 0.0 : f.start - f.gap);
        return std::clamp(desired, std::min(lowest, f.start), std::max(f.start, f.end - minLength));
    }
    // Senza ripple la fine nella sequenza si sposta: non oltre l'inizio del pezzo dopo.
    double highest = sourceDuration > 0 ? sourceDuration : std::numeric_limits<double>::max();
    if (!ripple && static_cast<std::size_t>(self) + 1 < fragments_.size())
        highest = std::min(highest, f.end + fragments_[static_cast<std::size_t>(self) + 1].gap);
    return std::clamp(desired, std::min(f.start + minLength, f.end), std::max(highest, f.end));
}

bool Sequence::trim(int fragmentId, Edge edge, double value, double sourceDuration, double minLength, bool ripple)
{
    const int self = indexOf(fragmentId);
    if (self < 0)
        return false;
    value = trimLimit(fragmentId, edge, value, sourceDuration, minLength, ripple);
    Fragment& f = fragments_[static_cast<std::size_t>(self)];
    const double old = edge == Edge::Start ? f.start : f.end;
    if (std::abs(value - old) <= kTimeEpsilon)
        return false;
    const double delta = value - old;
    if (edge == Edge::Start) {
        f.start = value;
        if (!ripple)
            f.gap = std::max(0.0, f.gap + delta);  // la fine resta ferma nella sequenza
    } else {
        f.end = value;
        if (!ripple && static_cast<std::size_t>(self) + 1 < fragments_.size()) {
            Fragment& next = fragments_[static_cast<std::size_t>(self) + 1];
            next.gap = std::max(0.0, next.gap - delta);  // il pezzo dopo resta fermo
            if (next.gap <= kTimeEpsilon)
                next.gap = 0.0;
        }
    }
    if (f.gap <= kTimeEpsilon)
        f.gap = 0.0;
    // Tratti senza audio: solo la parte dentro il pezzo.
    std::vector<TimeRange> kept;
    for (const TimeRange& r : f.muted) {
        const double from = std::max(r.start, f.start);
        const double to = std::min(r.end, f.end);
        if (to - from > kTimeEpsilon)
            kept.push_back({from, to});
    }
    f.muted = std::move(kept);
    return true;
}

bool Sequence::shiftPieces(const std::vector<std::pair<int, double>>& deltas)
{
    std::vector<double> at;
    double t = 0.0;
    for (const Fragment& f : fragments_) {
        t += f.gap;
        at.push_back(t);
        t += f.length();
    }
    for (const auto& [id, delta] : deltas) {
        const int index = indexOf(id);
        if (index < 0)
            return false;
        at[static_cast<std::size_t>(index)] += delta;
    }
    double previousEnd = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        if (at[i] < previousEnd - kTimeEpsilon)
            return false;  // si sovrappone al pezzo prima (o va prima di 0)
        previousEnd = at[i] + fragments_[i].length();
    }
    previousEnd = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        fragments_[i].gap = at[i] - previousEnd <= kTimeEpsilon ? 0.0 : at[i] - previousEnd;
        previousEnd = at[i] + fragments_[i].length();
    }
    return true;
}

bool Sequence::moveTo(int fragmentId, double start, std::vector<std::pair<int, double>>* shifts, bool push)
{
    const int self = indexOf(fragmentId);
    if (self < 0 || start < -kTimeEpsilon)
        return false;
    start = std::max(0.0, start);
    const double length = fragments_[self].length();
    // Posizioni di tutti i pezzi.
    std::vector<double> at;
    double t = 0.0;
    for (const Fragment& f : fragments_) {
        t += f.gap;
        at.push_back(t);
        t += f.length();
    }
    const double oldStart = at[static_cast<std::size_t>(self)];
    std::vector<std::pair<double, Fragment>> placed;
    if (push && start > oldStart + kTimeEpsilon) {
        // Verso destra: i pezzi dopo, se il pezzo li urta, vengono spinti avanti (a catena); quelli prima restano.
        if (self > 0 && start < at[static_cast<std::size_t>(self) - 1] + fragments_[static_cast<std::size_t>(self) - 1].length() - kTimeEpsilon)
            return false;
        double cursor = start + length;
        for (std::size_t i = 0; i < fragments_.size(); ++i) {
            double position = at[i];
            if (static_cast<int>(i) == self) {
                position = start;
            } else if (static_cast<int>(i) > self) {
                position = std::max(position, cursor);
                cursor = position + fragments_[i].length();
            }
            placed.emplace_back(position, fragments_[i]);
        }
    } else {
        // Senza spinta, o verso sinistra (o fermo): il pezzo non si sovrappone a nessun altro e può passare in un altro spazio.
        for (std::size_t i = 0; i < fragments_.size(); ++i) {
            const double end = at[i] + fragments_[i].length();
            if (static_cast<int>(i) != self) {
                if (start < end - kTimeEpsilon && start + length > at[i] + kTimeEpsilon)
                    return false;
                placed.emplace_back(at[i], fragments_[i]);
            }
        }
        placed.emplace_back(start, fragments_[static_cast<std::size_t>(self)]);
        std::stable_sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    if (shifts != nullptr) {
        shifts->clear();
        for (const auto& [position, f] : placed) {
            const int index = indexOf(f.id);
            if (std::abs(position - at[static_cast<std::size_t>(index)]) > kTimeEpsilon)
                shifts->emplace_back(f.id, position - at[static_cast<std::size_t>(index)]);
        }
    }
    fragments_.clear();
    double previousEnd = 0.0;
    for (auto& [position, f] : placed) {
        f.gap = std::max(0.0, position - previousEnd);
        if (f.gap <= kTimeEpsilon)
            f.gap = 0.0;
        previousEnd = previousEnd + f.gap + f.length();
        fragments_.push_back(std::move(f));
    }
    return true;
}

int Sequence::gapIndexAt(double seconds) const
{
    double t = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        const Fragment& f = fragments_[i];
        if (f.gap > kTimeEpsilon && seconds >= t - kTimeEpsilon && seconds < t + f.gap - kTimeEpsilon)
            return static_cast<int>(i);
        t += f.gap + f.length();
    }
    return -1;
}

bool Sequence::closeGap(int index)
{
    if (index < 0 || index >= static_cast<int>(fragments_.size()) || fragments_[index].gap <= 0.0)
        return false;
    fragments_[index].gap = 0.0;
    return true;
}

void Sequence::closeAllGaps()
{
    for (Fragment& f : fragments_)
        f.gap = 0.0;
}

int Sequence::gapCount() const
{
    return static_cast<int>(
        std::count_if(fragments_.begin(), fragments_.end(), [](const Fragment& f) { return f.gap > kTimeEpsilon; }));
}

void Sequence::clear()
{
    fragments_.clear();
}

void Sequence::restore(std::vector<Fragment> fragments)
{
    fragments_ = std::move(fragments);
    for (Fragment& f : fragments_) {
        f.id = nextId_++;
        f.gap = std::max(0.0, f.gap);
    }
}

bool Sequence::setDetached(int fragmentId, bool detached)
{
    for (Fragment& f : fragments_) {
        if (f.id == fragmentId) {
            if (f.detached == detached)
                return false;
            f.detached = detached;
            return true;
        }
    }
    return false;
}

bool Sequence::setEnvelope(int fragmentId, std::vector<LevelPoint> points)
{
    for (Fragment& f : fragments_) {
        if (f.id != fragmentId)
            continue;
        points = normalizeEnvelope(std::move(points));
        if (f.envelope == points)
            return false;
        f.envelope = std::move(points);
        return true;
    }
    return false;
}

int Sequence::split(double seconds)
{
    const int index = indexAt(seconds);
    if (index < 0)
        return 0;
    Fragment& f = fragments_[index];
    const double cut = f.start + (seconds - startOf(index));
    if (cut <= f.start + kTimeEpsilon || cut >= f.end - kTimeEpsilon)
        return 0;  // sul bordo del pezzo: non resterebbe niente da una parte
    Fragment second{nextId_++, f.clipId, cut, f.end, clipRanges(f.muted, cut, f.end)};
    second.envelope = f.envelope;  // la linea è nei tempi del file: ognuno tiene la sua parte
    f.end = cut;
    f.muted = clipRanges(f.muted, f.start, cut);
    fragments_.insert(fragments_.begin() + index + 1, second);
    return second.id;
}

Sequence::RemoveResult Sequence::removeSourceRange(int clipId, double from, double to)
{
    if (to <= from + kTimeEpsilon)
        return RemoveResult::Empty;

    bool touched = false;
    std::vector<Fragment> result;
    double carriedGap = 0.0;  // spazio vuoto prima di un pezzo tolto del tutto: passa al pezzo dopo
    for (const Fragment& f : fragments_) {
        if (f.clipId != clipId || f.end <= from + kTimeEpsilon || f.start >= to - kTimeEpsilon) {
            result.push_back(f);
            result.back().gap += carriedGap;
            carriedGap = 0.0;
            continue;
        }
        touched = true;
        const std::size_t before = result.size();
        if (f.start < from - kTimeEpsilon) {
            result.push_back({f.id, f.clipId, f.start, from, clipRanges(f.muted, f.start, from)});
            result.back().envelope = f.envelope;
        }
        if (f.end > to + kTimeEpsilon) {
            const bool firstPartKept = f.start < from - kTimeEpsilon;
            result.push_back({firstPartKept ? nextId_++ : f.id, f.clipId, to, f.end, clipRanges(f.muted, to, f.end)});
            result.back().envelope = f.envelope;
        }
        if (result.size() > before) {
            result[before].gap = f.gap + carriedGap;
            carriedGap = 0.0;
        } else {
            carriedGap += f.gap;
        }
    }
    if (!touched)
        return RemoveResult::OutsideKept;

    fragments_ = std::move(result);
    return RemoveResult::Removed;
}

bool Sequence::applyToRange(double from, double to, RangeOp op)
{
    if (to <= from + kTimeEpsilon)
        return false;
    bool changed = false;
    double start = 0.0;
    for (Fragment& f : fragments_) {
        start += f.gap;
        const double end = start + f.length();
        const double a = std::max(from, start);
        const double b = std::min(to, end);
        if (b > a + kTimeEpsilon) {
            const auto before = f.muted;
            op(f.muted, f.start + (a - start), f.start + (b - start));
            changed = changed || f.muted != before;
            if (f.muted != before && op == subtractRange)
                f.detached = false;  // l'audio torna a sentirsi: non è più scollegato
        }
        start = end;
    }
    return changed;
}

bool Sequence::muteRange(double from, double to)
{
    return applyToRange(from, to, addRange);
}

bool Sequence::unmuteRange(double from, double to)
{
    return applyToRange(from, to, subtractRange);
}

std::vector<TimeRange> Sequence::mutedRanges(bool includeDetached) const
{
    std::vector<TimeRange> result;
    double start = 0.0;
    for (const Fragment& f : fragments_) {
        start += f.gap;
        for (const TimeRange& r : f.muted) {
            if (f.detached && !includeDetached)
                break;
            addRange(result, start + (r.start - f.start), start + (r.end - f.start));
        }
        start += f.length();
    }
    return result;
}

const Fragment* Sequence::find(int fragmentId) const
{
    const int index = indexOf(fragmentId);
    return index < 0 ? nullptr : &fragments_[index];
}

int Sequence::indexOf(int fragmentId) const
{
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        if (fragments_[i].id == fragmentId)
            return static_cast<int>(i);
    }
    return -1;
}

double Sequence::totalLength() const
{
    double total = 0.0;
    for (const Fragment& f : fragments_)
        total += f.gap + f.length();
    return total;
}

double Sequence::startOf(int index) const
{
    double start = 0.0;
    for (int i = 0; i < index && i < static_cast<int>(fragments_.size()); ++i)
        start += fragments_[i].gap + fragments_[i].length();
    if (index >= 0 && index < static_cast<int>(fragments_.size()))
        start += fragments_[index].gap;
    return start;
}

std::vector<double> Sequence::starts() const
{
    std::vector<double> result;
    result.reserve(fragments_.size());
    double start = 0.0;
    for (const Fragment& f : fragments_) {
        start += f.gap;
        result.push_back(start);
        start += f.length();
    }
    return result;
}

int Sequence::indexAt(double seconds) const
{
    double start = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        start += fragments_[i].gap;
        const double end = start + fragments_[i].length();
        if (seconds >= start - kTimeEpsilon && seconds < end - kTimeEpsilon)
            return static_cast<int>(i);
        start = end;
    }
    return -1;
}

}  // namespace editor::core
