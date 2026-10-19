#include "EditorCore/Sequence.h"

#include <algorithm>

namespace editor::core {

namespace {

constexpr double kEpsilon = 0.001;

}  // namespace

int Sequence::insert(int index, int clipId, double start, double end, const SnapFunction& snap)
{
    // Un pezzo vuoto o al contrario si rifiuta prima dell'aggancio, che altrimenti lo allungherebbe all'indietro.
    if (end <= start + kEpsilon)
        return 0;
    const double snapped = snap(clipId, std::max(0.0, start));
    if (end <= snapped + kEpsilon)
        return 0;
    Fragment fragment{nextId_++, clipId, snapped, end};
    const auto at = index < 0 || index > static_cast<int>(fragments_.size()) ? fragments_.end()
                                                                              : fragments_.begin() + index;
    fragments_.insert(at, fragment);
    return fragment.id;
}

bool Sequence::remove(int fragmentId)
{
    const int index = indexOf(fragmentId);
    if (index < 0)
        return false;
    fragments_.erase(fragments_.begin() + index);
    return true;
}

void Sequence::removeClip(int clipId)
{
    std::erase_if(fragments_, [clipId](const Fragment& f) { return f.clipId == clipId; });
}

void Sequence::move(int fragmentId, int toIndex)
{
    const int from = indexOf(fragmentId);
    if (from < 0)
        return;
    toIndex = std::clamp(toIndex, 0, static_cast<int>(fragments_.size()) - 1);
    const Fragment fragment = fragments_[from];
    fragments_.erase(fragments_.begin() + from);
    fragments_.insert(fragments_.begin() + toIndex, fragment);
}

void Sequence::clear()
{
    fragments_.clear();
}

int Sequence::split(double seconds, const SnapFunction& snap)
{
    const int index = indexAt(seconds);
    if (index < 0)
        return 0;
    Fragment& f = fragments_[index];
    const double source = f.start + (seconds - startOf(index));
    const double cut = snap(f.clipId, source);
    if (cut <= f.start + kEpsilon || cut >= f.end - kEpsilon)
        return 0;  // nessun fotogramma chiave dentro il pezzo prima del cursore
    Fragment second{nextId_++, f.clipId, cut, f.end};
    f.end = cut;
    fragments_.insert(fragments_.begin() + index + 1, second);
    return second.id;
}

SegmentList::RemoveResult Sequence::removeSourceRange(int clipId, double from, double to, const SnapFunction& snap)
{
    if (to <= from + kEpsilon)
        return SegmentList::RemoveResult::Empty;

    bool touched = false;
    std::vector<Fragment> result;
    for (const Fragment& f : fragments_) {
        if (f.clipId != clipId || f.end <= from + kEpsilon || f.start >= to - kEpsilon) {
            result.push_back(f);
            continue;
        }
        touched = true;
        if (f.start < from - kEpsilon)
            result.push_back({f.id, f.clipId, f.start, from});
        // La parte dopo il pezzo tolto deve ripartire da un fotogramma chiave.
        const double restart = std::max(f.start, snap(clipId, to));
        if (f.end > to + kEpsilon && f.end > restart + kEpsilon) {
            const bool firstPartKept = f.start < from - kEpsilon;
            result.push_back({firstPartKept ? nextId_++ : f.id, f.clipId, restart, f.end});
        }
    }
    if (!touched)
        return SegmentList::RemoveResult::OutsideKept;

    // Se la parte dopo riparte da prima del pezzo da togliere, non si toglie niente.
    double before = 0.0;
    double after = 0.0;
    for (const Fragment& f : fragments_)
        before += f.length();
    for (const Fragment& f : result)
        after += f.length();
    if (after >= before - kEpsilon)
        return SegmentList::RemoveResult::NoKeyframeInside;

    fragments_ = std::move(result);
    return SegmentList::RemoveResult::Removed;
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
        total += f.length();
    return total;
}

double Sequence::startOf(int index) const
{
    double start = 0.0;
    for (int i = 0; i < index && i < static_cast<int>(fragments_.size()); ++i)
        start += fragments_[i].length();
    return start;
}

int Sequence::indexAt(double seconds) const
{
    double start = 0.0;
    for (std::size_t i = 0; i < fragments_.size(); ++i) {
        const double end = start + fragments_[i].length();
        if (seconds >= start - kEpsilon && seconds < end - kEpsilon)
            return static_cast<int>(i);
        start = end;
    }
    return -1;
}

}  // namespace editor::core
