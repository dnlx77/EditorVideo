#include "EditorCore/TimelineZoom.h"

#include <algorithm>

namespace editor::core {

namespace {

// Margine a sinistra quando si passa alla pagina successiva: il cursore non riparte proprio dal bordo.
constexpr double kFollowMargin = 0.05;

}  // namespace

void TimelineZoom::setDuration(double seconds)
{
    duration_ = std::max(0.0, seconds);
    if (!fit_ && pixelsPerSecond_ <= fitPixelsPerSecond())
        fit();
}

void TimelineZoom::setWidth(double pixels)
{
    width_ = std::max(1.0, pixels);
    if (!fit_ && pixelsPerSecond_ <= fitPixelsPerSecond())
        fit();
}

void TimelineZoom::setMaxPixelsPerSecond(double pixelsPerSecond)
{
    maxPixelsPerSecond_ = std::max(1.0, pixelsPerSecond);
    if (!fit_)
        pixelsPerSecond_ = std::min(pixelsPerSecond_, maxPixelsPerSecond());
}

double TimelineZoom::fitPixelsPerSecond() const
{
    return duration_ > 0 ? width_ / duration_ : 1.0;
}

double TimelineZoom::maxPixelsPerSecond() const
{
    return std::max(maxPixelsPerSecond_, fitPixelsPerSecond());
}

double TimelineZoom::pixelsPerSecond() const
{
    return fit_ ? fitPixelsPerSecond() : std::clamp(pixelsPerSecond_, fitPixelsPerSecond(), maxPixelsPerSecond());
}

bool TimelineZoom::isFit() const
{
    return fit_;
}

double TimelineZoom::maxOffset() const
{
    return std::max(0.0, duration_ - visibleSeconds());
}

double TimelineZoom::offset() const
{
    return fit_ ? 0.0 : std::clamp(offset_, 0.0, maxOffset());
}

double TimelineZoom::visibleSeconds() const
{
    return width_ / pixelsPerSecond();
}

double TimelineZoom::contentWidth() const
{
    return duration_ * pixelsPerSecond();
}

double TimelineZoom::xForTime(double seconds) const
{
    return (seconds - offset()) * pixelsPerSecond();
}

double TimelineZoom::timeForX(double x) const
{
    return std::clamp(offset() + x / pixelsPerSecond(), 0.0, duration_);
}

void TimelineZoom::zoomAt(double factor, double x)
{
    if (duration_ <= 0 || factor <= 0)
        return;
    const double anchor = offset() + x / pixelsPerSecond();  // il tempo che deve restare in x
    const double wanted = std::min(pixelsPerSecond() * factor, maxPixelsPerSecond());
    if (wanted <= fitPixelsPerSecond() * (1.0 + 1e-9)) {
        fit();
        return;
    }
    fit_ = false;
    pixelsPerSecond_ = wanted;
    offset_ = std::clamp(anchor - x / pixelsPerSecond_, 0.0, maxOffset());
}

void TimelineZoom::fit()
{
    fit_ = true;
    offset_ = 0.0;
}

void TimelineZoom::scrollTo(double offsetSeconds)
{
    if (!fit_)
        offset_ = std::clamp(offsetSeconds, 0.0, maxOffset());
}

void TimelineZoom::scrollBy(double pixels)
{
    scrollTo(offset() + pixels / pixelsPerSecond());
}

bool TimelineZoom::follow(double seconds)
{
    if (fit_)
        return false;
    const double start = offset();
    const double visible = visibleSeconds();
    if (seconds >= start && seconds < start + visible)
        return false;
    scrollTo(seconds - visible * kFollowMargin);
    return offset() != start;
}

}  // namespace editor::core
