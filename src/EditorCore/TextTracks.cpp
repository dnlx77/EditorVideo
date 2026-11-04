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
    return index < 0 ? std::string() : "V" + std::to_string(index + 2);
}

bool TextTracks::empty() const
{
    return std::all_of(layers_.begin(), layers_.end(), [](const TextLayer& l) { return l.clips.empty() && l.pieces.empty(); });
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
    double position = 0.0;
    double end = 0.0;
    const TextLayer* target = layer(layerId);
    if (!extentOf(clipId, position, end) || target == nullptr)
        return -1.0;
    const double length = end - position;
    desired = std::max(desired, 0.0);
    if (isFree(*target, desired, desired + length, clipId))
        return desired;

    // I posti liberi sono gli spazi fra gli altri oggetti (e dopo l'ultimo): in ognuno si prende il punto più vicino.
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
    for (const auto& [from, to] : occupied(*target, clipId)) {
        consider(cursor, from);
        cursor = std::max(cursor, to);
    }
    consider(cursor, std::numeric_limits<double>::max() / 4);
    return best;
}

bool TextTracks::moveTo(int clipId, int layerId, double start)
{
    if (findPiece(clipId) != nullptr)
        return movePieceTo(clipId, layerId, start);
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
    second.fadeIn = 0.0;  // la dissolvenza in entrata resta alla prima parte, quella in uscita alla seconda
    clip->fadeOut = 0.0;
    clip->length = seconds - clip->position;
    TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOf(clipId)))];
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
    const int index = layerIndex(layerId);
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
    const int toIndex = layerIndex(layerId);
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
    if (!isFree(l, position, position + (sourceEnd - sourceStart), pieceId))
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
    second.start = piece->start + (seconds - piece->position);
    second.id = nextClipId_++;
    piece->end = second.start;
    TextLayer& l = layers_[static_cast<std::size_t>(layerIndex(layerOfPiece(pieceId)))];
    l.pieces.push_back(std::move(second));
    sortLayer(l);
    return nextClipId_ - 1;
}

bool TextTracks::setPieceTransform(int pieceId, const Transform& transform)
{
    LayerPiece* piece = findPieceMutable(pieceId);
    if (piece == nullptr || piece->transform == transform)
        return false;
    piece->transform = transform;
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
        for (const LayerPiece& p : l.pieces)
            end = std::max(end, p.endPosition());
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
