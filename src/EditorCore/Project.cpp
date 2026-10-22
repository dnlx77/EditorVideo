#include "EditorCore/Project.h"

#include "EditorCore/TimeUtil.h"

#include <algorithm>

namespace editor::core {

double Clip::snapToKeyframe(double seconds) const
{
    return core::snapToKeyframe(keyframes, seconds);
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
    const int index = indexOf(id);
    if (index < 0)
        return;
    Clip& clip = clips_[index];
    clip.keyframes = std::move(keyframes);
    std::sort(clip.keyframes.begin(), clip.keyframes.end());
    clip.duration = std::max(0.0, duration);
    clip.analyzed = true;
}

void Project::setClipPath(int id, std::string path)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    Clip& clip = clips_[index];
    clip.path = std::move(path);
    clip.analyzed = false;
    clip.keyframes.clear();
    clip.duration = 0.0;
}

void Project::remove(int id)
{
    std::erase_if(clips_, [id](const Clip& c) { return c.id == id; });
    sequence_.removeClip(id);
}

void Project::clear()
{
    clips_.clear();
    sequence_.clear();
    replacement_.reset();
    format_ = {};
}

const Clip* Project::find(int id) const
{
    const int index = indexOf(id);
    return index < 0 ? nullptr : &clips_[index];
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

Sequence::SnapFunction Project::snapper() const
{
    return [](int, double seconds) { return seconds; };
}

std::vector<ClipRanges> Project::exportPlan(GapExport gaps) const
{
    std::vector<ClipRanges> plan;
    int previousClip = 0;
    for (const Fragment& f : sequence_.fragments()) {
        const Clip* clip = find(f.clipId);
        if (clip == nullptr)
            continue;
        if (plan.empty() || f.clipId != previousClip)
            plan.push_back({clip->path, {}, clip->keyframes});
        plan.back().ranges.push_back({f.start, f.end});
        plan.back().gapsBefore.push_back(gaps == GapExport::Black ? f.gap : 0.0);
        previousClip = f.clipId;
    }
    return plan;
}

void Project::setAudioReplacement(std::optional<AudioReplacement> replacement)
{
    replacement_ = std::move(replacement);
}

void Project::setAudioOffset(double offset)
{
    if (replacement_)
        replacement_->offset = offset;
}

void Project::restoreEdits(const Project& snapshot)
{
    std::vector<Clip> clips = snapshot.clips_;
    for (Clip& clip : clips) {
        const Clip* now = find(clip.id);
        if (now != nullptr && now->analyzed && now->path == clip.path) {
            clip.keyframes = now->keyframes;
            clip.duration = now->duration;
            clip.analyzed = true;
        }
    }
    clips_ = std::move(clips);
    sequence_ = snapshot.sequence_;
    replacement_ = snapshot.replacement_;
    format_ = snapshot.format_;
    nextId_ = std::max(nextId_, snapshot.nextId_);  // un file aggiunto dopo non riprende l'id di uno tolto
}

AudioEdit Project::audioEdit(GapExport gaps) const
{
    if (gaps == GapExport::Black)
        return {sequence_.mutedRanges(), replacement_};  // i tempi della sequenza, spazi compresi
    // GapExport::Close: i tempi della sequenza senza gli spazi vuoti, come nel file esportato.
    Sequence closed = sequence_;
    closed.closeAllGaps();
    return {closed.mutedRanges(), replacement_};
}

}  // namespace editor::core
