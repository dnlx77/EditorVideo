#include "EditorCore/SegmentList.h"

#include <algorithm>

namespace editor::core {

namespace {

constexpr double kEpsilon = 0.001;

}  // namespace

void SegmentList::reset(std::vector<double> keyframes, double duration)
{
    keyframes_ = std::move(keyframes);
    std::sort(keyframes_.begin(), keyframes_.end());
    duration_ = std::max(0.0, duration);
    ranges_.clear();
}

double SegmentList::clampToFile(double seconds) const
{
    return duration_ > 0 ? std::clamp(seconds, 0.0, duration_) : std::max(0.0, seconds);
}

double SegmentList::snapToKeyframe(double seconds) const
{
    if (keyframes_.empty())
        return seconds;
    const auto next = std::upper_bound(keyframes_.begin(), keyframes_.end(), seconds + kEpsilon);
    return next == keyframes_.begin() ? keyframes_.front() : *std::prev(next);
}

void SegmentList::normalize()
{
    // Via i tratti vuoti, poi in ordine e uniti quelli che si toccano o si sovrappongono.
    std::erase_if(ranges_, [](const TimeRange& r) { return r.end <= r.start + kEpsilon; });
    std::sort(ranges_.begin(), ranges_.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : ranges_) {
        if (!merged.empty() && r.start <= merged.back().end + kEpsilon)
            merged.back().end = std::max(merged.back().end, r.end);
        else
            merged.push_back(r);
    }
    ranges_ = std::move(merged);
}

bool SegmentList::add(double in, double out)
{
    // Un tratto vuoto o al contrario si rifiuta prima dell'aggancio, che altrimenti lo allungherebbe all'indietro.
    if (clampToFile(out) <= clampToFile(in) + kEpsilon)
        return false;
    const double start = snapToKeyframe(clampToFile(in));
    const double end = clampToFile(out);
    if (end <= start + kEpsilon)
        return false;
    ranges_.push_back({start, end});
    normalize();
    return true;
}

bool SegmentList::remove(double in, double out)
{
    const double cutStart = clampToFile(in);
    const double cutEnd = clampToFile(out);
    if (cutEnd <= cutStart + kEpsilon)
        return false;

    const std::vector<TimeRange> before = ranges_;
    if (ranges_.empty())
        ranges_.push_back({snapToKeyframe(0.0), duration_});

    std::vector<TimeRange> result;
    for (const TimeRange& r : ranges_) {
        if (r.end <= cutStart + kEpsilon || r.start >= cutEnd - kEpsilon) {
            result.push_back(r);  // non tocca il pezzo tolto
            continue;
        }
        if (r.start < cutStart - kEpsilon)
            result.push_back({r.start, cutStart});
        // La parte dopo il pezzo tolto deve ripartire da un fotogramma chiave.
        if (r.end > cutEnd + kEpsilon)
            result.push_back({std::max(r.start, snapToKeyframe(cutEnd)), r.end});
    }
    ranges_ = std::move(result);
    normalize();

    if (ranges_ == before || (before.empty() && ranges_.size() == 1 && ranges_.front().start <= snapToKeyframe(0.0) + kEpsilon
                              && ranges_.front().end >= duration_ - kEpsilon)) {
        ranges_ = before;  // niente di fatto (es. nessun fotogramma chiave dentro il pezzo da togliere)
        return false;
    }
    return true;
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
        if (seconds >= ranges_[i].start - kEpsilon && seconds < ranges_[i].end - kEpsilon)
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
