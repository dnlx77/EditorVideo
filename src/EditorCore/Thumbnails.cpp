#include "EditorCore/Thumbnails.h"

namespace editor::core {

ThumbnailExtractor::ThumbnailExtractor(int maxWidth, int maxHeight, bool allowHardware)
    : maxWidth_(maxWidth)
    , maxHeight_(maxHeight)
    , allowHardware_(allowHardware)
{
}

ThumbnailExtractor::~ThumbnailExtractor() = default;

std::shared_ptr<VideoFrame> ThumbnailExtractor::extract(const std::string& utf8Path, double seconds)
{
    if (utf8Path != path_) {
        decoder_.reset();
        path_ = utf8Path;
        failed_ = false;
        try {
            decoder_ = std::make_unique<VideoDecoder>(
                utf8Path, VideoDecoder::Options{.allowHardware = allowHardware_, .maxWidth = maxWidth_, .maxHeight = maxHeight_,
                                                .readAhead = false});
        } catch (const std::exception&) {
            failed_ = true;  // niente video (es. un file solo audio)
        }
    }
    if (failed_ || !decoder_)
        return nullptr;
    try {
        return decoder_->seek(seconds);
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::string ThumbnailExtractor::decoderDescription() const
{
    return decoder_ ? decoder_->decoderDescription() : std::string();
}

}  // namespace editor::core
