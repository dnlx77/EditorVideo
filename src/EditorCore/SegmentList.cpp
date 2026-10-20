#include "EditorCore/SegmentList.h"

#include "EditorCore/TimeUtil.h"

#include <algorithm>

namespace editor::core {

void SegmentList::reset(std::vector<double> keyframes, double duration)
{
    keyframes_ = std::move(keyframes);
    std::sort(keyframes_.begin(), keyframes_.end());
    duration_ = std::max(0.0, duration);
    ranges_.clear();
}

double SegmentList::clampToFile(double seconds) const
{
    return clampTo(seconds, duration_);
}

double SegmentList::snapToKeyframe(double seconds) const
{
    return core::snapToKeyframe(keyframes_, seconds);
}

void SegmentList::normalize()
{
    // Via i tratti vuoti, poi in ordine e uniti quelli che si toccano o si sovrappongono.
    std::erase_if(ranges_, [](const TimeRange& r) { return r.end <= r.start + kTimeEpsilon; });
    std::sort(ranges_.begin(), ranges_.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : ranges_) {
        if (!merged.empty() && r.start <= merged.back().end + kTimeEpsilon)
            merged.back().end = std::max(merged.back().end, r.end);
        else
            merged.push_back(r);
    }
    ranges_ = std::move(merged);
}

bool SegmentList::add(double in, double out)
{
    // Un tratto vuoto o al contrario si rifiuta prima dell'aggancio, che altrimenti lo allungherebbe all'indietro.
    if (clampToFile(out) <= clampToFile(in) + kTimeEpsilon)
        return false;
    const double start = snapToKeyframe(clampToFile(in));
    const double end = clampToFile(out);
    if (end <= start + kTimeEpsilon)
        return false;
    ranges_.push_back({start, end});
    normalize();
    return true;
}

SegmentList::RemoveResult SegmentList::remove(double in, double out)
{
    const double cutStart = clampToFile(in);
    const double cutEnd = clampToFile(out);
    if (cutEnd <= cutStart + kTimeEpsilon)
        return RemoveResult::Empty;

    const std::vector<TimeRange> before = ranges_;
    if (ranges_.empty())
        ranges_.push_back({snapToKeyframe(0.0), duration_});

    const bool touchesKept = std::any_of(ranges_.begin(), ranges_.end(), [&](const TimeRange& r) {
        return r.end > cutStart + kTimeEpsilon && r.start < cutEnd - kTimeEpsilon;
    });
    if (!touchesKept) {
        ranges_ = before;
        return RemoveResult::OutsideKept;
    }

    std::vector<TimeRange> result;
    for (const TimeRange& r : ranges_) {
        if (r.end <= cutStart + kTimeEpsilon || r.start >= cutEnd - kTimeEpsilon) {
            result.push_back(r);  // non tocca il pezzo tolto
            continue;
        }
        if (r.start < cutStart - kTimeEpsilon)
            result.push_back({r.start, cutStart});
        // La parte dopo il pezzo tolto deve ripartire da un fotogramma chiave.
        if (r.end > cutEnd + kTimeEpsilon)
            result.push_back({std::max(r.start, snapToKeyframe(cutEnd)), r.end});
    }
    ranges_ = std::move(result);
    normalize();

    if (ranges_ == before
        || (before.empty() && ranges_.size() == 1 && ranges_.front().start <= snapToKeyframe(0.0) + kTimeEpsilon
            && ranges_.front().end >= duration_ - kTimeEpsilon)) {
        ranges_ = before;  // niente di fatto: la parte dopo ripartirebbe da prima del pezzo da togliere
        return RemoveResult::NoKeyframeInside;
    }
    return RemoveResult::Removed;
}

bool SegmentList::removeAt(double seconds)
{
    const int index = indexAt(seconds);
    if (index < 0)
        return false;
    ranges_.erase(ranges_.begin() + index);
    return true;
}

void SegmentList::clear()
{
    ranges_.clear();
}

int SegmentList::indexAt(double seconds) const
{
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
        if (seconds >= ranges_[i].start - kTimeEpsilon && seconds < ranges_[i].end - kTimeEpsilon)
            return static_cast<int>(i);
    }
    return -1;
}

double SegmentList::totalLength() const
{
    double total = 0.0;
    for (const TimeRange& r : ranges_)
        total += r.length();
    return total;
}

}  // namespace editor::core
