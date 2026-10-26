#include "EditorCore/TextTracks.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace editor::core {

namespace {

constexpr double kEps = 1e-9;

}  // namespace

int TextTracks::addLayer()
{
    TextLayer layer;
    layer.id = nextLayerId_++;
    layers_.push_back(std::move(layer));
    return layers_.back().id;
}

bool TextTracks::removeLayer(int layerId)
{
    return std::erase_if(layers_, [layerId](const TextLayer& l) { return l.id == layerId; }) > 0;
}

const TextLayer* TextTracks::layer(int layerId) const
{
    const int index = layerIndex(layerId);
    return index < 0 ? nullptr : &layers_[static_cast<std::size_t>(index)];
}

int TextTracks::layerIndex(int layerId) const
{
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].id == layerId)
            return static_cast<int>(i);
    }
    return -1;
}

std::string TextTracks::layerName(int layerId) const
{
    const int index = layerIndex(layerId);
    return index < 0 ? std::string() : "T" + std::to_string(index + 1);
}

bool TextTracks::empty() const
{
    return std::all_of(layers_.begin(), layers_.end(), [](const TextLayer& l) { return l.clips.empty(); });
}

bool TextTracks::isFree(const TextLayer& layer, double start, double end, int ignoreId) const
{
    for (const TextClip& c : layer.clips) {
        if (c.id != ignoreId && start < c.endPosition() - kEps && end > c.position + kEps)
            return false;
    }
    return true;
}

void TextTracks::sortLayer(TextLayer& layer) const
{
    std::stable_sort(layer.clips.begin(), layer.clips.end(),
                     [](const TextClip& a, const TextClip& b) { return a.position < b.position; });
}

TextClip* TextTracks::findMutable(int clipId)
{
    for (TextLayer& l : layers_) {
        for (TextClip& c : l.clips) {
            if (c.id == clipId)
                return &c;
        }
    }
    return nullptr;
}

int TextTracks::add(int layerId, TextClip clip)
{
    const int index = layerIndex(layerId);
    if (index < 0 || !(clip.length >= kMinLength) || !(clip.position >= 0.0))
        return 0;
    TextLayer& target = layers_[static_cast<std::size_t>(index)];
    if (!isFree(target, clip.position, clip.endPosition(), 0))
        return 0;
    clip.id = nextClipId_++;
    target.clips.push_back(std::move(clip));
    sortLayer(target);
    return nextClipId_ - 1;
}

bool TextTracks::remove(int clipId)
{
    for (TextLayer& l : layers_) {
        if (std::erase_if(l.clips, [clipId](const TextClip& c) { return c.id == clipId; }) > 0)
            return true;
    }
    return false;
}

const TextClip* TextTracks::find(int clipId) const
{
    for (const TextLayer& l : layers_) {
        for (const TextClip& c : l.clips) {
            if (c.id == clipId)
                return &c;
        }
    }
    return nullptr;
}

int TextTracks::layerOf(int clipId) const
{
    for (const TextLayer& l : layers_) {
        for (const TextClip& c : l.clips) {
            if (c.id == clipId)
                return l.id;
        }
    }
    return -1;
}

double TextTracks::nearestFreeStart(int clipId, int layerId, double desired) const
{
    const TextClip* clip = find(clipId);
    const TextLayer* target = layer(layerId);
    if (clip == nullptr || target == nullptr)
        return -1.0;
    const double length = clip->length;
    desired = std::max(desired, 0.0);
    if (isFree(*target, desired, desired + length, clipId))
        return desired;

    // I posti liberi sono gli spazi fra le altre scritte (e dopo l'ultima): in ognuno si prende il punto più vicino.
    std::vector<const TextClip*> others;
    for (const TextClip& c : target->clips) {
        if (c.id != clipId)
            others.push_back(&c);
    }
    double best = -1.0;
    double bestDistance = std::numeric_limits<double>::max();
    const auto consider = [&](double gapStart, double gapEnd) {
        if (gapEnd - gapStart < length - kEps)
            return;
        const double start = std::clamp(desired, gapStart, gapEnd - length);
        const double distance = std::abs(start - desired);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = start;
        }
    };
    double cursor = 0.0;
    for (const TextClip* o : others) {
        consider(cursor, o->position);
        cursor = std::max(cursor, o->endPosition());
    }
    consider(cursor, std::numeric_limits<double>::max() / 4);
    return best;
}

bool TextTracks::moveTo(int clipId, int layerId, double start)
{
    TextClip* clip = findMutable(clipId);
    const int toIndex = layerIndex(layerId);
    if (clip == nullptr || toIndex < 0 || !(start >= 0.0))
        return false;
    TextLayer& to = layers_[static_cast<std::size_t>(toIndex)];
    if (!isFree(to, start, start + clip->length, clipId))
        return false;
    if (layerOf(clipId) == layerId) {
        clip->position = start;
        sortLayer(to);
        return true;
    }
    TextClip moved = *clip;
    moved.position = start;
    remove(clipId);
    to.clips.push_back(std::move(moved));
    sortLayer(to);
    return true;
}

bool TextTracks::setStart(int clipId, double start)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || !(start >= 0.0))
        return false;
    const double end = clip->endPosition();
    if (end - start < kMinLength - kEps)
        return false;
    const TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOf(clipId)))];
    if (!isFree(l, start, end, clipId))
        return false;
    clip->length = end - start;
    clip->position = start;
    return true;
}

bool TextTracks::setEnd(int clipId, double end)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || end - clip->position < kMinLength - kEps)
        return false;
    const TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOf(clipId)))];
    if (!isFree(l, clip->position, end, clipId))
        return false;
    clip->length = end - clip->position;
    return true;
}

bool TextTracks::setLength(int clipId, double length)
{
    const TextClip* clip = find(clipId);
    return clip != nullptr && setEnd(clipId, clip->position + length);
}

int TextTracks::split(int clipId, double seconds)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || seconds - clip->position < kMinLength - kEps || clip->endPosition() - seconds < kMinLength - kEps)
        return 0;
    TextClip second = *clip;
    second.position = seconds;
    second.length = clip->endPosition() - seconds;
    second.id = nextClipId_++;
    clip->length = seconds - clip->position;
    TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOf(clipId)))];
    l.clips.push_back(std::move(second));
    sortLayer(l);
    return nextClipId_ - 1;
}

bool TextTracks::setText(int clipId, std::string text)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || clip->text == text)
        return false;
    clip->text = std::move(text);
    return true;
}

bool TextTracks::setStyle(int clipId, const TextStyle& style)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || clip->style == style)
        return false;
    clip->style = style;
    return true;
}

std::vector<const TextClip*> TextTracks::visibleAt(double seconds) const
{
    std::vector<const TextClip*> visible;
    for (const TextLayer& l : layers_) {
        for (const TextClip& c : l.clips) {
            if (seconds >= c.position && seconds < c.endPosition())
                visible.push_back(&c);
        }
    }
    return visible;
}

double TextTracks::endTime() const
{
    double end = 0.0;
    for (const TextLayer& l : layers_) {
        for (const TextClip& c : l.clips)
            end = std::max(end, c.endPosition());
    }
    return end;
}

void TextTracks::clear()
{
    layers_.clear();
}

void TextTracks::restore(std::vector<TextLayer> layers)
{
    layers_.clear();
    for (TextLayer& in : layers) {
        const int layerId = addLayer();
        for (TextClip& clip : in.clips)
            add(layerId, std::move(clip));
    }
}

void TextTracks::restoreEdits(const TextTracks& snapshot)
{
    const int nextLayer = std::max(nextLayerId_, snapshot.nextLayerId_);
    const int nextClip = std::max(nextClipId_, snapshot.nextClipId_);
    *this = snapshot;
    nextLayerId_ = nextLayer;
    nextClipId_ = nextClip;
}

}  // namespace editor::core
