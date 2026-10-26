#include "EditorCore/TextFrame.h"

#include "EditorCore/MediaError.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

using detail::FramePtr;

TextFrameBlender::TextFrameBlender(int width, int height)
    : width_(width)
    , height_(height)
    , rgba_(static_cast<std::size_t>(width) * height * 4)
    , software_(av_frame_alloc())
    , output_(av_frame_alloc())
{
    if (!software_ || !output_)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    output_->format = AV_PIX_FMT_YUV420P;
    output_->width = width;
    output_->height = height;
    if (av_frame_get_buffer(output_.get(), 0) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
}

const AVFrame* TextFrameBlender::blend(const AVFrame* frame, const std::vector<const TextImage*>& texts)
{
    const AVFrame* source = frame;
    if (frame->hw_frames_ctx != nullptr) {
        av_frame_unref(software_.get());
        if (const int ret = av_hwframe_transfer_data(software_.get(), frame, 0); ret < 0)
            throw MediaError("Fotogramma della scheda video non leggibile: " + detail::ffmpegError(ret));
        av_frame_copy_props(software_.get(), frame);
        source = software_.get();
    }

    // Misura dell'immagine dentro il fotogramma del progetto: stesse regole dell'encoder (proporzioni dell'originale,
    // bande nere, posizioni pari).
    const AVRational sar = source->sample_aspect_ratio.num > 0 ? source->sample_aspect_ratio : AVRational{1, 1};
    const double aspect = static_cast<double>(source->width) * sar.num / (static_cast<double>(source->height) * sar.den);
    int width = width_;
    int height = height_;
    if (aspect > static_cast<double>(width_) / height_ + 1e-3)
        height = std::min(height_, static_cast<int>(std::lround(width_ / aspect / 2.0)) * 2);
    else if (aspect < static_cast<double>(width_) / height_ - 1e-3)
        width = std::min(width_, static_cast<int>(std::lround(height_ * aspect / 2.0)) * 2);
    const int x = (width_ - width) / 2 & ~1;
    const int y = (height_ - height) / 2 & ~1;

    // Come l'encoder: i formati "J" sono a gamma piena.
    AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(source->format);
    bool fullRange = source->color_range == AVCOL_RANGE_JPEG;
    switch (sourceFormat) {
    case AV_PIX_FMT_YUVJ420P: sourceFormat = AV_PIX_FMT_YUV420P; fullRange = true; break;
    case AV_PIX_FMT_YUVJ422P: sourceFormat = AV_PIX_FMT_YUV422P; fullRange = true; break;
    case AV_PIX_FMT_YUVJ444P: sourceFormat = AV_PIX_FMT_YUV444P; fullRange = true; break;
    default: break;
    }
    toRgba_.reset(sws_getCachedContext(toRgba_.release(), source->width, source->height, sourceFormat, width, height,
                                       AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
    toYuv_.reset(sws_getCachedContext(toYuv_.release(), width_, height_, AV_PIX_FMT_RGBA, width_, height_, AV_PIX_FMT_YUV420P,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!toRgba_ || !toYuv_)
        throw MediaError("Conversione del fotogramma da codificare non possibile.");
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(toRgba_.get(), coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    sws_setColorspaceDetails(toYuv_.get(), coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);

    // Nero opaco dappertutto, poi l'immagine al suo posto.
    for (std::size_t i = 0; i < rgba_.size(); i += 4) {
        rgba_[i] = rgba_[i + 1] = rgba_[i + 2] = 0;
        rgba_[i + 3] = 255;
    }
    std::uint8_t* destination[4] = {rgba_.data() + (static_cast<std::ptrdiff_t>(y) * width_ + x) * 4, nullptr, nullptr, nullptr};
    const int destinationStride[4] = {width_ * 4, 0, 0, 0};
    sws_scale(toRgba_.get(), source->data, source->linesize, 0, source->height, destination, destinationStride);

    for (const TextImage* text : texts) {
        if (text != nullptr)
            blendTextOver(rgba_.data(), width_ * 4, width_, height_, *text);
    }

    if (av_frame_make_writable(output_.get()) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    const std::uint8_t* rgbaPlanes[4] = {rgba_.data(), nullptr, nullptr, nullptr};
    const int rgbaStride[4] = {width_ * 4, 0, 0, 0};
    sws_scale(toYuv_.get(), rgbaPlanes, rgbaStride, 0, height_, output_->data, output_->linesize);
    av_frame_copy_props(output_.get(), source);
    output_->sample_aspect_ratio = {1, 1};
    output_->pts = frame->pts;
    output_->color_range = AVCOL_RANGE_MPEG;
    return output_.get();
}

}  // namespace editor::core
