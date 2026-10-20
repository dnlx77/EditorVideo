#include "EditorCore/CutSegment.h"

#include "EditorCore/TimeUtil.h"

#include <algorithm>

namespace editor::core {

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
    return clampTo(seconds, duration_);
}

double CutSegment::snapToKeyframe(double seconds) const
{
    return core::snapToKeyframe(keyframes_, seconds);
}

double CutSegment::setIn(double seconds)
{
    in_ = snapToKeyframe(clampToFile(seconds));
    hasIn_ = true;
    if (hasOut_ && out_ <= in_ + kTimeEpsilon)
        hasOut_ = false;
    return in_;
}

bool CutSegment::setOut(double seconds)
{
    const double out = clampToFile(seconds);
    if (out <= in() + kTimeEpsilon)
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
