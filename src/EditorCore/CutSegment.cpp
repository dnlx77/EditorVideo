#include "EditorCore/CutSegment.h"

#include <algorithm>

namespace editor::core {

namespace {

// Tolleranza sui confronti fra tempi: i tempi dei fotogrammi arrivano da calcoli in virgola mobile.
constexpr double kEpsilon = 0.001;

}  // namespace

void CutSegment::reset(std::vector<double> keyframes, double duration)
{
    keyframes_ = std::move(keyframes);
    std::sort(keyframes_.begin(), keyframes_.end());
    duration_ = std::max(0.0, duration);
    hasIn_ = false;
    hasOut_ = false;
    in_ = 0.0;
    out_ = 0.0;
}

double CutSegment::clampToFile(double seconds) const
{
    return duration_ > 0 ? std::clamp(seconds, 0.0, duration_) : std::max(0.0, seconds);
}

double CutSegment::snapToKeyframe(double seconds) const
{
    if (keyframes_.empty())
        return seconds;
    // Primo fotogramma chiave strettamente dopo `seconds` (con tolleranza): quello prima è il nostro.
    const auto next = std::upper_bound(keyframes_.begin(), keyframes_.end(), seconds + kEpsilon);
    return next == keyframes_.begin() ? keyframes_.front() : *std::prev(next);
}

double CutSegment::setIn(double seconds)
{
    in_ = snapToKeyframe(clampToFile(seconds));
    hasIn_ = true;
    if (hasOut_ && out_ <= in_ + kEpsilon)
        hasOut_ = false;
    return in_;
}

bool CutSegment::setOut(double seconds)
{
    const double out = clampToFile(seconds);
    if (out <= in() + kEpsilon)
        return false;
    out_ = out;
    hasOut_ = true;
    return true;
}

void CutSegment::clearIn()
{
    hasIn_ = false;
}

void CutSegment::clearOut()
{
    hasOut_ = false;
}

double CutSegment::in() const
{
    return hasIn_ ? in_ : 0.0;
}

double CutSegment::out() const
{
    return hasOut_ ? out_ : duration_;
}

double CutSegment::length() const
{
    return std::max(0.0, out() - in());
}

}  // namespace editor::core
