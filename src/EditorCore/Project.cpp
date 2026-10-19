#include "EditorCore/Project.h"

#include <algorithm>

namespace editor::core {

std::vector<TimeRange> Clip::exportRanges() const
{
    if (!analyzed)
        return {};
    if (!segments.empty())
        return segments.ranges();
    if (segments.duration() <= 0)
        return {};
    // Il file intero, dal primo fotogramma chiave.
    const double start = segments.snapToKeyframe(0.0);
    if (start >= segments.duration())
        return {};
    return {{start, segments.duration()}};
}

double Clip::outputLength() const
{
    double total = 0.0;
    for (const TimeRange& r : exportRanges())
        total += r.length();
    return total;
}

int Project::add(std::string path)
{
    Clip clip;
    clip.id = nextId_++;
    clip.path = std::move(path);
    clips_.push_back(std::move(clip));
    return clips_.back().id;
}

void Project::setAnalysis(int id, std::vector<double> keyframes, double duration)
{
    if (Clip* clip = find(id)) {
        clip->segments.reset(std::move(keyframes), duration);
        clip->analyzed = true;
    }
}

void Project::remove(int id)
{
    std::erase_if(clips_, [id](const Clip& c) { return c.id == id; });
}

void Project::move(int id, int to)
{
    const int from = indexOf(id);
    if (from < 0)
        return;
    to = std::clamp(to, 0, static_cast<int>(clips_.size()) - 1);
    Clip clip = std::move(clips_[from]);
    clips_.erase(clips_.begin() + from);
    clips_.insert(clips_.begin() + to, std::move(clip));
}

void Project::clear()
{
    clips_.clear();
}

Clip* Project::find(int id)
{
    const auto it = std::find_if(clips_.begin(), clips_.end(), [id](const Clip& c) { return c.id == id; });
    return it == clips_.end() ? nullptr : &*it;
}

const Clip* Project::find(int id) const
{
    return const_cast<Project*>(this)->find(id);
}

int Project::indexOf(int id) const
{
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

bool Project::allAnalyzed() const
{
    return std::all_of(clips_.begin(), clips_.end(), [](const Clip& c) { return c.analyzed; });
}

std::vector<ClipRanges> Project::exportPlan() const
{
    std::vector<ClipRanges> plan;
    for (const Clip& clip : clips_) {
        std::vector<TimeRange> ranges = clip.exportRanges();
        if (!ranges.empty())
            plan.push_back({clip.path, std::move(ranges)});
    }
    return plan;
}

double Project::totalLength() const
{
    double total = 0.0;
    for (const Clip& clip : clips_)
        total += clip.outputLength();
    return total;
}

}  // namespace editor::core
