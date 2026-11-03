#include "EditorCore/TextFrame.h"

#include "EditorCore/Compositor.h"
#include "EditorCore/MediaError.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

using detail::FramePtr;

TextFrameBlender::TextFrameBlender(int width, int height, AVPixelFormat pixelFormat)
    : width_(width)
    , height_(height)
    , pixelFormat_(pixelFormat)
    , rgba_(static_cast<std::size_t>(width) * height * 4)
    , software_(av_frame_alloc())
    , output_(av_frame_alloc())
{
    if (!software_ || !output_)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    output_->format = pixelFormat_;
    output_->width = width;
    output_->height = height;
    if (av_frame_get_buffer(output_.get(), 0) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
}

const AVFrame* TextFrameBlender::blend(const AVFrame* frame, const std::vector<BlendedText>& texts)
{
    const AVFrame* source = frame;
    if (frame->hw_frames_ctx != nullptr) {
        if (const int ret = detail::downloadHardwareFrame(software_.get(), frame); ret < 0)
            throw MediaError("Fotogramma della scheda video non leggibile: " + detail::ffmpegError(ret));
        source = software_.get();
    }

    // Misura dell'immagine dentro il fotogramma del progetto: le stesse regole dell'encoder (proporzioni dell'originale,
    // bande nere, posizioni pari).
    const detail::FittedSize fit = detail::fitInto(source, width_, height_);
    const int width = fit.width;
    const int height = fit.height;
    const int x = fit.x;
    const int y = fit.y;

    // Come l'encoder: i formati "J" sono a gamma piena.
    AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(source->format);
    bool fullRange = source->color_range == AVCOL_RANGE_JPEG;
    detail::normalizeJpegRange(sourceFormat, fullRange);
    toRgba_.reset(sws_getCachedContext(toRgba_.release(), source->width, source->height, sourceFormat, width, height,
                                       AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
    toYuv_.reset(sws_getCachedContext(toYuv_.release(), width_, height_, AV_PIX_FMT_RGBA, width_, height_, pixelFormat_,
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

    return toYuv(texts, frame);
}

const AVFrame* TextFrameBlender::blendTransformed(const AVFrame* frame, const Transform& transform,
                                                  const std::vector<BlendedText>& texts)
{
    if (transform.isIdentity())
        return blend(frame, texts);
    const AVFrame* source = frame;
    if (frame->hw_frames_ctx != nullptr) {
        if (const int ret = detail::downloadHardwareFrame(software_.get(), frame); ret < 0)
            throw MediaError("Fotogramma della scheda video non leggibile: " + detail::ffmpegError(ret));
        source = software_.get();
    }
    const int sourceWidth = source->width;
    const int sourceHeight = source->height;
    AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(source->format);
    bool fullRange = source->color_range == AVCOL_RANGE_JPEG;
    detail::normalizeJpegRange(sourceFormat, fullRange);
    // Il fotogramma originale in RGBA alla sua misura (la qualità piena, non quella adattata al progetto).
    toRgba_.reset(sws_getCachedContext(toRgba_.release(), sourceWidth, sourceHeight, sourceFormat, sourceWidth, sourceHeight,
                                       AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
    toYuv_.reset(sws_getCachedContext(toYuv_.release(), width_, height_, AV_PIX_FMT_RGBA, width_, height_, pixelFormat_,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!toRgba_ || !toYuv_)
        throw MediaError("Conversione del fotogramma da codificare non possibile.");
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(toRgba_.get(), coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    sws_setColorspaceDetails(toYuv_.get(), coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);

    layer_.resize(static_cast<std::size_t>(sourceWidth) * sourceHeight * 4);
    std::uint8_t* layerPlanes[4] = {layer_.data(), nullptr, nullptr, nullptr};
    const int layerStride[4] = {sourceWidth * 4, 0, 0, 0};
    sws_scale(toRgba_.get(), source->data, source->linesize, 0, sourceHeight, layerPlanes, layerStride);

    for (std::size_t i = 0; i < rgba_.size(); i += 4) {
        rgba_[i] = rgba_[i + 1] = rgba_[i + 2] = 0;
        rgba_[i + 3] = 255;
    }
    // compositeLayer non guarda i canali: RGBA si comporta come BGRA.
    compositeLayer(rgba_.data(), width_ * 4, width_, height_, BgraImage{layer_.data(), sourceWidth * 4, sourceWidth, sourceHeight},
                   transform);
    return toYuv(texts, frame);
}

const AVFrame* TextFrameBlender::blendRaster(const std::uint8_t* bgra, int stride, int width, int height,
                                             const std::vector<BlendedText>& texts, const AVFrame* props)
{
    if (width != width_ || height != height_)
        throw MediaError("La transizione non ha la misura del fotogramma del progetto.");
    for (int y = 0; y < height_; ++y) {
        const std::uint8_t* in = bgra + static_cast<std::ptrdiff_t>(y) * stride;
        std::uint8_t* out = rgba_.data() + static_cast<std::size_t>(y) * width_ * 4;
        for (int x = 0; x < width_; ++x, in += 4, out += 4) {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = 255;
        }
    }
    toYuv_.reset(sws_getCachedContext(toYuv_.release(), width_, height_, AV_PIX_FMT_RGBA, width_, height_, pixelFormat_,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!toYuv_)
        throw MediaError("Conversione del fotogramma da codificare non possibile.");
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(toYuv_.get(), coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
    return toYuv(texts, props);
}

const AVFrame* TextFrameBlender::toYuv(const std::vector<BlendedText>& texts, const AVFrame* props)
{
    for (const BlendedText& text : texts) {
        if (text.image != nullptr)
            blendTextOver(rgba_.data(), width_ * 4, width_, height_, *text.image, text.opacity);
    }

    if (av_frame_make_writable(output_.get()) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    const std::uint8_t* rgbaPlanes[4] = {rgba_.data(), nullptr, nullptr, nullptr};
    const int rgbaStride[4] = {width_ * 4, 0, 0, 0};
    sws_scale(toYuv_.get(), rgbaPlanes, rgbaStride, 0, height_, output_->data, output_->linesize);
    av_frame_copy_props(output_.get(), props);
    output_->sample_aspect_ratio = {1, 1};
    output_->pts = props->pts;
    output_->color_range = AVCOL_RANGE_MPEG;
    return output_.get();
}

}  // namespace editor::core
