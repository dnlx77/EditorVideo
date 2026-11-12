#include "EditorCore/TextTracks.h"
#include "EditorCore/TimeUtil.h"

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
    if (layerId == kBaseLayerId)
        return &base_;
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
    if (layerId == kBaseLayerId)
        return "V1";
    const int index = layerIndex(layerId);
    return index < 0 ? std::string() : "V" + std::to_string(index + 2);
}

bool TextTracks::empty() const
{
    return base_.clips.empty()
           && std::all_of(layers_.begin(), layers_.end(), [](const TextLayer& l) { return l.clips.empty() && l.pieces.empty(); });
}

bool TextTracks::isFree(const TextLayer& layer, double start, double end, int ignoreId) const
{
    for (const auto& [from, to] : occupied(layer, ignoreId)) {
        if (start < to - kEps && end > from + kEps)
            return false;
    }
    return true;
}

std::vector<std::pair<double, double>> TextTracks::occupied(const TextLayer& layer, int ignoreId) const
{
    std::vector<std::pair<double, double>> spans;
    for (const TextClip& c : layer.clips) {
        if (c.id != ignoreId)
            spans.emplace_back(c.position, c.endPosition());
    }
    for (const LayerPiece& p : layer.pieces) {
        if (p.id != ignoreId)
            spans.emplace_back(p.position, p.endPosition());
    }
    if (&layer == &base_)
        spans.insert(spans.end(), baseBlocked_.begin(), baseBlocked_.end());
    std::sort(spans.begin(), spans.end());
    return spans;
}

bool TextTracks::extentOf(int id, double& position, double& end) const
{
    if (const TextClip* clip = find(id)) {
        position = clip->position;
        end = clip->endPosition();
        return true;
    }
    if (const LayerPiece* piece = findPiece(id)) {
        position = piece->position;
        end = piece->endPosition();
        return true;
    }
    return false;
}

int TextTracks::layerOfObject(int id) const
{
    const int text = layerOf(id);
    return text >= 0 ? text : layerOfPiece(id);
}

void TextTracks::sortLayer(TextLayer& layer) const
{
    std::stable_sort(layer.clips.begin(), layer.clips.end(),
                     [](const TextClip& a, const TextClip& b) { return a.position < b.position; });
    std::stable_sort(layer.pieces.begin(), layer.pieces.end(),
                     [](const LayerPiece& a, const LayerPiece& b) { return a.position < b.position; });
}

TextLayer* TextTracks::layerMutable(int layerId)
{
    if (layerId == kBaseLayerId)
        return &base_;
    const int index = layerIndex(layerId);
    return index < 0 ? nullptr : &layers_[static_cast<std::size_t>(index)];
}

std::vector<TextLayer*> TextTracks::allLayers()
{
    std::vector<TextLayer*> all{&base_};
    for (TextLayer& l : layers_)
        all.push_back(&l);
    return all;
}

std::vector<const TextLayer*> TextTracks::allLayers() const
{
    std::vector<const TextLayer*> all{&base_};
    for (const TextLayer& l : layers_)
        all.push_back(&l);
    return all;
}

TextClip* TextTracks::findMutable(int clipId)
{
    for (TextLayer* l : allLayers()) {
        for (TextClip& c : l->clips) {
            if (c.id == clipId)
                return &c;
        }
    }
    return nullptr;
}

int TextTracks::add(int layerId, TextClip clip)
{
    TextLayer* found = layerMutable(layerId);
    if (found == nullptr || !(clip.length >= kMinLength) || !(clip.position >= 0.0))
        return 0;
    TextLayer& target = *found;
    if (!isFree(target, clip.position, clip.endPosition(), 0))
        return 0;
    clip.id = nextClipId_++;
    target.clips.push_back(std::move(clip));
    sortLayer(target);
    return nextClipId_ - 1;
}

bool TextTracks::remove(int clipId)
{
    for (TextLayer* l : allLayers()) {
        if (std::erase_if(l->clips, [clipId](const TextClip& c) { return c.id == clipId; }) > 0)
            return true;
    }
    return false;
}

const TextClip* TextTracks::find(int clipId) const
{
    for (const TextLayer* l : allLayers()) {
        for (const TextClip& c : l->clips) {
            if (c.id == clipId)
                return &c;
        }
    }
    return nullptr;
}

int TextTracks::layerOf(int clipId) const
{
    for (const TextLayer* l : allLayers()) {
        for (const TextClip& c : l->clips) {
            if (c.id == clipId)
                return l->id;
        }
    }
    return -1;
}

double TextTracks::nearestFreeStart(int clipId, int layerId, double desired) const
{
    double position = 0.0;
    double end = 0.0;
    if (!extentOf(clipId, position, end))
        return -1.0;
    return freeStartFor(layerId, end - position, desired, clipId);
}

double TextTracks::freeStartFor(int layerId, double length, double desired, int ignoreId) const
{
    const TextLayer* target = layer(layerId);
    if (target == nullptr)
        return -1.0;
    std::vector<TimeRange> spans;
    for (const auto& [from, to] : occupied(*target, ignoreId))
        spans.push_back({from, to});
    return closestFreeStart(std::move(spans), length, desired, kEps);
}

bool TextTracks::moveTo(int clipId, int layerId, double start)
{
    if (findPiece(clipId) != nullptr)
        return movePieceTo(clipId, layerId, start);
    TextClip* clip = findMutable(clipId);
    TextLayer* toLayer = layerMutable(layerId);
    if (clip == nullptr || toLayer == nullptr || !(start >= 0.0))
        return false;
    TextLayer& to = *toLayer;
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

bool TextTracks::canPlace(int layerId, double start, double length) const
{
    const TextLayer* target = layer(layerId);
    return target != nullptr && start >= 0.0 && length >= kMinLength && isFree(*target, start, start + length, 0);
}

double TextTracks::edgeLimit(int clipId, bool atStart, double desired) const
{
    double position = 0.0;
    double end = 0.0;
    const int layerId = layerOfObject(clipId);
    if (!extentOf(clipId, position, end) || layerId < 0)
        return -1.0;
    double before = 0.0;                                      // fine dell'oggetto che precede
    double after = std::numeric_limits<double>::infinity();  // inizio di quello che segue
    for (const auto& [from, to] : occupied(*layer(layerId), clipId)) {
        if (to <= position + kEps)
            before = std::max(before, to);
        else if (from >= end - kEps)
            after = std::min(after, from);
    }
    if (atStart)
        return std::clamp(desired, before, end - kMinLength);
    return std::clamp(desired, position + kMinLength, after);
}

bool TextTracks::setStart(int clipId, double start)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || !(start >= 0.0))
        return false;
    const double end = clip->endPosition();
    if (end - start < kMinLength - kEps)
        return false;
    const TextLayer& l = *layer(layerOf(clipId));
    if (!isFree(l, start, end, clipId))
        return false;
    shiftKeys(clip->animation, clip->position - start);  // le chiavi restano attaccate all'immagine
    clip->length = end - start;
    clip->position = start;
    return true;
}

bool TextTracks::setEnd(int clipId, double end)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || end - clip->position < kMinLength - kEps)
        return false;
    const TextLayer& l = *layer(layerOf(clipId));
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
    shiftKeys(second.animation, -(seconds - clip->position));  // i tempi della seconda parte partono dal suo inizio
    second.fadeIn = 0.0;  // la dissolvenza in entrata resta alla prima parte, quella in uscita alla seconda
    clip->fadeOut = 0.0;
    clip->length = seconds - clip->position;
    TextLayer& l = *layerMutable(layerOf(clipId));
    l.clips.push_back(std::move(second));
    sortLayer(l);
    return nextClipId_ - 1;
}

bool TextTracks::setFades(int clipId, double fadeIn, double fadeOut)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || !std::isfinite(fadeIn) || !std::isfinite(fadeOut))
        return false;
    fadeIn = std::clamp(fadeIn, 0.0, clip->length);
    fadeOut = std::clamp(fadeOut, 0.0, clip->length);
    if (clip->fadeIn == fadeIn && clip->fadeOut == fadeOut)
        return false;
    clip->fadeIn = fadeIn;
    clip->fadeOut = fadeOut;
    return true;
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

bool TextTracks::setTransform(int clipId, const Transform& transform)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr || clip->transform == transform)
        return false;
    clip->transform = transform;
    return true;
}

bool TextTracks::setAnimation(int clipId, const TransformAnimation& animation)
{
    TextClip* clip = findMutable(clipId);
    if (clip == nullptr)
        return false;
    TransformAnimation value = animation;
    normalizeAnimation(value);
    if (clip->animation == value)
        return false;
    clip->animation = std::move(value);
    return true;
}

// ---- Pezzi di video sui livelli

LayerPiece* TextTracks::findPieceMutable(int pieceId)
{
    for (TextLayer& l : layers_) {
        for (LayerPiece& p : l.pieces) {
            if (p.id == pieceId)
                return &p;
        }
    }
    return nullptr;
}

const LayerPiece* TextTracks::findPiece(int pieceId) const
{
    for (const TextLayer& l : layers_) {
        for (const LayerPiece& p : l.pieces) {
            if (p.id == pieceId)
                return &p;
        }
    }
    return nullptr;
}

int TextTracks::layerOfPiece(int pieceId) const
{
    for (const TextLayer& l : layers_) {
        for (const LayerPiece& p : l.pieces) {
            if (p.id == pieceId)
                return l.id;
        }
    }
    return -1;
}

int TextTracks::addPiece(int layerId, LayerPiece piece)
{
    const int index = layerIndex(layerId);  // (V1 non ha pezzi: -1)
    if (index < 0 || !(piece.length() >= kMinLength) || !(piece.position >= 0.0) || !(piece.start >= 0.0))
        return 0;
    TextLayer& target = layers_[static_cast<std::size_t>(index)];
    if (!isFree(target, piece.position, piece.endPosition(), 0))
        return 0;
    piece.id = nextClipId_++;
    target.pieces.push_back(std::move(piece));
    sortLayer(target);
    return nextClipId_ - 1;
}

bool TextTracks::removePiece(int pieceId)
{
    for (TextLayer& l : layers_) {
        if (std::erase_if(l.pieces, [pieceId](const LayerPiece& p) { return p.id == pieceId; }) > 0)
            return true;
    }
    return false;
}

bool TextTracks::movePieceTo(int pieceId, int layerId, double start)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    const int toIndex = layerIndex(layerId);  // (V1 non ha pezzi: -1)
    if (piece == nullptr || toIndex < 0 || !(start >= 0.0))
        return false;
    TextLayer& to = layers_[static_cast<std::size_t>(toIndex)];
    if (!isFree(to, start, start + piece->length(), pieceId))
        return false;
    if (layerOfPiece(pieceId) == layerId) {
        piece->position = start;
        sortLayer(to);
        return true;
    }
    LayerPiece moved = *piece;
    moved.position = start;
    removePiece(pieceId);
    to.pieces.push_back(std::move(moved));
    sortLayer(to);
    return true;
}

bool TextTracks::trimPiece(int pieceId, double position, double sourceStart, double sourceEnd)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    if (piece == nullptr || !(position >= 0.0) || !(sourceStart >= 0.0) || !(sourceEnd - sourceStart >= kMinLength - kEps))
        return false;
    TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOfPiece(pieceId)))];
    if (!isFree(l, position, position + (sourceEnd - sourceStart) / piece->speed, pieceId))
        return false;
    piece->position = position;
    piece->start = sourceStart;
    piece->end = sourceEnd;
    sortLayer(l);
    return true;
}

int TextTracks::splitPiece(int pieceId, double seconds)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    if (piece == nullptr || seconds - piece->position < kMinLength - kEps || piece->endPosition() - seconds < kMinLength - kEps)
        return 0;
    LayerPiece second = *piece;
    second.position = seconds;
    const SpeedMap map = piece->map();
    const TimeRange firstPart = map.fileRangeOf(0.0, seconds - piece->position);
    const TimeRange secondPart = map.fileRangeOf(seconds - piece->position, map.length());
    second.start = secondPart.start;
    second.end = secondPart.end;
    second.id = nextClipId_++;
    piece->start = firstPart.start;
    piece->end = firstPart.end;
    TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOfPiece(pieceId)))];
    l.pieces.push_back(std::move(second));
    sortLayer(l);
    return nextClipId_ - 1;
}

bool TextTracks::setPieceSpeed(int pieceId, double speed, bool reverse)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    if (piece == nullptr)
        return false;
    speed = clampSpeed(speed);
    if (std::abs(piece->speed - speed) < 1e-9 && piece->reverse == reverse)
        return false;
    TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOfPiece(pieceId)))];
    if (!isFree(l, piece->position, piece->position + (piece->end - piece->start) / speed, pieceId))
        return false;
    piece->speed = speed;
    piece->reverse = reverse;
    return true;
}

bool TextTracks::setPieceTransform(int pieceId, const Transform& transform)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    if (piece == nullptr || piece->transform == transform)
        return false;
    piece->transform = transform;
    return true;
}

bool TextTracks::setPieceAnimation(int pieceId, const TransformAnimation& animation)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    if (piece == nullptr)
        return false;
    TransformAnimation value = animation;
    normalizeAnimation(value);
    if (piece->animation == value)
        return false;
    piece->animation = std::move(value);
    return true;
}

std::vector<const LayerPiece*> TextTracks::piecesVisibleAt(double seconds) const
{
    std::vector<const LayerPiece*> visible;
    for (const TextLayer& l : layers_) {
        for (const LayerPiece& p : l.pieces) {
            if (seconds >= p.position && seconds < p.endPosition())
                visible.push_back(&p);
        }
    }
    return visible;
}

std::vector<const TextClip*> TextTracks::visibleAt(double seconds) const
{
    std::vector<const TextClip*> visible;
    for (const TextLayer* l : allLayers()) {
        for (const TextClip& c : l->clips) {
            if (seconds >= c.position && seconds < c.endPosition())
                visible.push_back(&c);
        }
    }
    return visible;
}

double TextTracks::endTime() const
{
    double end = 0.0;
    for (const TextLayer* l : allLayers()) {
        for (const TextClip& c : l->clips)
            end = std::max(end, c.endPosition());
        for (const LayerPiece& p : l->pieces)
            end = std::max(end, p.endPosition());
    }
    return end;
}

void TextTracks::clear()
{
    layers_.clear();
    base_.clips.clear();
}

void TextTracks::restore(std::vector<TextLayer> layers, std::vector<TextClip> baseClips)
{
    layers_.clear();
    base_.clips.clear();
    for (TextClip& clip : baseClips)
        add(kBaseLayerId, std::move(clip));
    for (TextLayer& in : layers) {
        const int layerId = addLayer();
        for (TextClip& clip : in.clips)
            add(layerId, std::move(clip));
        for (LayerPiece& piece : in.pieces)
            addPiece(layerId, std::move(piece));
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
