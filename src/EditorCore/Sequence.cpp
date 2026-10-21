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

int Sequence::insert(int index, int clipId, double start, double end, const SnapFunction& snap)
{
    // Un pezzo vuoto o al contrario si rifiuta prima dell'aggancio, che altrimenti lo allungherebbe all'indietro.
    if (end <= start + kTimeEpsilon)
        return 0;
    const double snapped = snap(clipId, std::max(0.0, start));
    if (end <= snapped + kTimeEpsilon)
        return 0;
    Fragment fragment{nextId_++, clipId, snapped, end};
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

double Sequence::nearestFreeStart(int fragmentId, double desired) const
{
    const int self = indexOf(fragmentId);
    if (self < 0)
        return -1.0;
    const double length = fragments_[self].length();
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
    double t = 0.0;
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

bool Sequence::moveTo(int fragmentId, double start)
{
    const int self = indexOf(fragmentId);
    if (self < 0 || start < -kTimeEpsilon)
        return false;
    start = std::max(0.0, start);
    const double length = fragments_[self].length();
    // Posizioni di tutti i pezzi; quello spostato non deve sovrapporsi a nessun altro.
    std::vector<std::pair<double, Fragment>> placed;
    double t = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        t += fragments_[i].gap;
        const double end = t + fragments_[i].length();
        if (static_cast<int>(i) != self) {
            if (start < end - kTimeEpsilon && start + length > t + kTimeEpsilon)
                return false;
            placed.emplace_back(t, fragments_[i]);
        }
        t = end;
    }
    placed.emplace_back(start, fragments_[self]);
    std::stable_sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    fragments_.clear();
    double previousEnd = 0.0;
    for (auto& [at, f] : placed) {
        f.gap = std::max(0.0, at - previousEnd);
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

int Sequence::split(double seconds, const SnapFunction& snap)
{
    const int index = indexAt(seconds);
    if (index < 0)
        return 0;
    Fragment& f = fragments_[index];
    const double source = f.start + (seconds - startOf(index));
    const double cut = snap(f.clipId, source);
    if (cut <= f.start + kTimeEpsilon || cut >= f.end - kTimeEpsilon)
        return 0;  // nessun fotogramma chiave dentro il pezzo prima del cursore
    Fragment second{nextId_++, f.clipId, cut, f.end, clipRanges(f.muted, cut, f.end)};
    f.end = cut;
    f.muted = clipRanges(f.muted, f.start, cut);
    fragments_.insert(fragments_.begin() + index + 1, second);
    return second.id;
}

Sequence::RemoveResult Sequence::removeSourceRange(int clipId, double from, double to, const SnapFunction& snap)
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
        if (f.start < from - kTimeEpsilon)
            result.push_back({f.id, f.clipId, f.start, from, clipRanges(f.muted, f.start, from)});
        // La parte dopo il pezzo tolto deve ripartire da un fotogramma chiave.
        const double restart = std::max(f.start, snap(clipId, to));
        if (f.end > to + kTimeEpsilon && f.end > restart + kTimeEpsilon) {
            const bool firstPartKept = f.start < from - kTimeEpsilon;
            result.push_back({firstPartKept ? nextId_++ : f.id, f.clipId, restart, f.end, clipRanges(f.muted, restart, f.end)});
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

    // Se la parte dopo riparte da prima del pezzo da togliere, non si toglie niente.
    double before = 0.0;
    double after = 0.0;
    for (const Fragment& f : fragments_)
        before += f.length();
    for (const Fragment& f : result)
        after += f.length();
    if (after >= before - kTimeEpsilon)
        return RemoveResult::NoKeyframeInside;

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

std::vector<TimeRange> Sequence::mutedRanges() const
{
    std::vector<TimeRange> result;
    double start = 0.0;
    for (const Fragment& f : fragments_) {
        start += f.gap;
        for (const TimeRange& r : f.muted)
            addRange(result, start + (r.start - f.start), start + (r.end - f.start));
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
