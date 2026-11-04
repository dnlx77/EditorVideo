#include "EditorCore/LayerFrames.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

namespace {
constexpr double kMaxSide = 4096.0;
constexpr double kJump = 2.0;  // oltre questa distanza (secondi) dal fotogramma tenuto ci si riposiziona invece di decodificare
}  // namespace

LayerFrameSource::LayerFrameSource(std::map<int, std::string> files, int projectWidth, int projectHeight, bool allowHardware)
    : files_(std::move(files))
    , width_(std::max(projectWidth, 16))
    , height_(std::max(projectHeight, 16))
    , hardware_(allowHardware)
{
}

LayerFrameSource::~LayerFrameSource() = default;

std::shared_ptr<VideoFrame> LayerFrameSource::frameFor(const LayerPiece& piece, double fileTime)
{
    Reader& reader = readers_[piece.id];
    if (reader.failed)
        return nullptr;
    if (!reader.decoder) {
        const auto file = files_.find(piece.clipId);
        if (file == files_.end()) {
            reader.failed = true;
            return nullptr;
        }
        try {
            VideoDecoder::Options options;
            options.allowHardware = hardware_;
            options.readAhead = false;
            // Un pezzo ingrandito si decodifica più grande, così non è sfocato (fino a una misura ragionevole).
            const double zoom = std::clamp(std::max(piece.transform.scaleX, piece.transform.scaleY), 1.0, 4.0);
            options.maxWidth = static_cast<int>(std::min(width_ * zoom, kMaxSide));
            options.maxHeight = static_cast<int>(std::min(height_ * zoom, kMaxSide));
            reader.decoder = std::make_unique<VideoDecoder>(file->second, options);
        } catch (const MediaError&) {
            reader.failed = true;
            return nullptr;
        }
    }
    const double ahead = reader.next ? reader.next->time : (reader.current ? reader.current->time : 0.0);
    const bool lost = !reader.current || fileTime < reader.current->time - 0.5 || fileTime > ahead + kJump;
    if (lost) {
        reader.current = reader.decoder->seek(fileTime);
        reader.next = reader.current ? reader.decoder->nextFrame() : nullptr;
        if (!reader.current)
            reader.failed = true;
        return reader.current;
    }
    // Avanti finché il fotogramma dopo non è oltre il tempo chiesto (con mezzo fotogramma di tolleranza per i bordi).
    while (reader.next && reader.next->time <= fileTime + 1e-4) {
        reader.current = reader.next;
        reader.next = reader.decoder->nextFrame();
    }
    return reader.current;
}

std::vector<LayerFrameSource::Visible> LayerFrameSource::at(const TextTracks& tracks, double sequenceTime)
{
    std::vector<Visible> visible;
    const std::vector<const LayerPiece*> pieces = tracks.piecesVisibleAt(sequenceTime);
    for (auto it = readers_.begin(); it != readers_.end();) {
        const bool stillVisible = std::any_of(pieces.begin(), pieces.end(), [&](const LayerPiece* p) { return p->id == it->first; });
        it = stillVisible ? std::next(it) : readers_.erase(it);
    }
    for (const LayerPiece* piece : pieces) {
        const double fileTime = std::clamp(piece->start + (sequenceTime - piece->position), piece->start, std::max(piece->start, piece->end - 1e-3));
        if (std::shared_ptr<VideoFrame> frame = frameFor(*piece, fileTime))
            visible.push_back({std::move(frame), piece->transform});
    }
    return visible;
}

}  // namespace editor::core
