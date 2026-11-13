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
            // (con i fotogrammi chiave, la scala più grande fra quelle fisse e quelle delle chiavi)
            double biggest = std::max(piece.transform.scaleX, piece.transform.scaleY);
            for (const AnimParam p : {AnimParam::ScaleX, AnimParam::ScaleY}) {
                for (const AnimKey& key : piece.animation.track(p))
                    biggest = std::max(biggest, key.value);
            }
            const double zoom = std::clamp(biggest, 1.0, 4.0);
            options.maxWidth = static_cast<int>(std::min(width_ * zoom, kMaxSide));
            options.maxHeight = static_cast<int>(std::min(height_ * zoom, kMaxSide));
            reader.decoder = std::make_unique<VideoDecoder>(file->second, options);
        } catch (const MediaError&) {
            reader.failed = true;
            return nullptr;
        }
    }
    if (piece.reverse)
        return reverseFrameFor(reader, fileTime);
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

std::shared_ptr<VideoFrame> LayerFrameSource::reverseFrameFor(Reader& reader, double fileTime)
{
    const double rate = reader.decoder->frameRate();
    const double duration = rate > 0.0 ? 1.0 / rate : 0.04;
    const auto pick = [&]() -> std::shared_ptr<VideoFrame> {
        std::shared_ptr<VideoFrame> best;
        for (const std::shared_ptr<VideoFrame>& f : reader.block) {
            if (f->time > fileTime + 1e-4)
                break;
            best = f;
        }
        return best;
    };
    if (!reader.block.empty() && fileTime >= reader.block.front()->time - 1e-4 && fileTime <= reader.block.back()->time + duration) {
        if (auto found = pick())
            return found;
    }
    // Nuovo blocco, che finisce poco oltre `fileTime`: ~150 MB di fotogrammi al massimo (da 4 a 40 fotogrammi).
    constexpr std::int64_t kBudget = 150LL << 20;
    std::int64_t bytes = 0;
    if (!reader.block.empty())
        bytes = static_cast<std::int64_t>(reader.block.front()->stride) * reader.block.front()->height;
    const int count = bytes > 0 ? static_cast<int>(std::clamp<std::int64_t>(kBudget / bytes, 4, 40)) : 12;
    reader.block.clear();
    try {
        const double from = std::max(0.0, fileTime - count * duration);
        for (std::shared_ptr<VideoFrame> f = reader.decoder->seek(from); f; f = reader.decoder->nextFrame()) {
            reader.block.push_back(f);
            if (f->time > fileTime + 1e-4)
                break;
        }
    } catch (const MediaError&) {
        reader.block.clear();
    }
    if (reader.block.empty()) {
        reader.failed = true;
        return nullptr;
    }
    if (auto found = pick())
        return found;
    return reader.block.front();
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
        // Con velocità e inverso il tempo del file è quello della mappa del pezzo (tenuto dentro il pezzo).
        const double fileTime = std::clamp(piece->map().fileAt(std::clamp(sequenceTime - piece->position, 0.0, piece->length())),
                                           piece->start, std::max(piece->start, piece->end - 1e-3));
        if (std::shared_ptr<VideoFrame> frame = frameFor(*piece, fileTime))
            visible.push_back({std::move(frame), evaluateTransform(piece->transform, piece->animation, fileTime), tracks.levelOf(piece->id)});
    }
    return visible;
}

}  // namespace editor::core
