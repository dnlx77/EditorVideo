#include "EditorCore/TextFrame.h"

#include "EditorCore/BandedSws.h"
#include "EditorCore/Compositor.h"
#include "EditorCore/MediaError.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace editor::core {

using detail::FramePtr;

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point from)
{
    return std::chrono::duration<double>(Clock::now() - from).count();
}

// Nero opaco (RGBA) su tutta l'immagine, a bande su più thread.
void fillOpaqueBlack(std::vector<std::uint8_t>& rgba, int width, int height)
{
    runBands(bandEdges(height, bandCount(height)), [&](int, int y0, int y1) {
        // Un pixel RGBA nero opaco, scritto come una parola da 32 bit (byte 0, 0, 0, 255).
        const std::uint8_t pixel[4] = {0, 0, 0, 255};
        std::uint32_t word = 0;
        std::memcpy(&word, pixel, sizeof word);
        std::uint32_t* p = reinterpret_cast<std::uint32_t*>(rgba.data()) + static_cast<std::size_t>(y0) * width;
        std::fill(p, p + static_cast<std::size_t>(y1 - y0) * width, word);
    });
}
// Il formato del fotogramma come lo vede la conversione: come l'encoder, i formati "J" diventano quelli normali a gamma piena.
AVPixelFormat normalizedFormat(const AVFrame* source, bool& fullRange)
{
    AVPixelFormat format = static_cast<AVPixelFormat>(source->format);
    fullRange = source->color_range == AVCOL_RANGE_JPEG;
    detail::normalizeJpegRange(format, fullRange);
    return format;
}
}  // namespace

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

const AVFrame* TextFrameBlender::softwareSource(const AVFrame* frame)
{
    if (frame->hw_frames_ctx == nullptr)
        return frame;
    if (const int ret = detail::downloadHardwareFrame(software_.get(), frame); ret < 0)
        throw MediaError("Fotogramma della scheda video non leggibile: " + detail::ffmpegError(ret));
    return software_.get();
}

const AVFrame* TextFrameBlender::blend(const AVFrame* frame, const std::vector<BlendedText>& texts)
{
    const AVFrame* source = softwareSource(frame);

    // Misura dell'immagine dentro il fotogramma del progetto: le stesse regole dell'encoder (proporzioni dell'originale,
    // bande nere, posizioni pari).
    const detail::FittedSize fit = detail::fitInto(source, width_, height_);
    const int width = fit.width;
    const int height = fit.height;
    const int x = fit.x;
    const int y = fit.y;

    bool fullRange = false;
    const AVPixelFormat sourceFormat = normalizedFormat(source, fullRange);
    toRgba_.reset(sws_getCachedContext(toRgba_.release(), source->width, source->height, sourceFormat, width, height,
                                       AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!toRgba_)
        throw MediaError("Conversione del fotogramma da codificare non possibile.");
    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(toRgba_.get(), coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);

    // Nero opaco dappertutto, poi l'immagine al suo posto.
    fillOpaqueBlack(rgba_, width_, height_);
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
    const AVFrame* source = softwareSource(frame);
    const int sourceWidth = source->width;
    const int sourceHeight = source->height;
    bool fullRange = false;
    const AVPixelFormat sourceFormat = normalizedFormat(source, fullRange);
    timing_.sourceFormat = sourceFormat;
    // Il fotogramma originale in RGBA alla sua misura (la qualità piena, non quella adattata al progetto).
    if (!bandedToRgba_.prepare(sourceWidth, sourceHeight, sourceFormat, AV_PIX_FMT_RGBA, fullRange, true))
        throw MediaError("Conversione del fotogramma da codificare non possibile.");

    auto mark = Clock::now();
    layer_.resize(static_cast<std::size_t>(sourceWidth) * sourceHeight * 4);
    std::uint8_t* layerPlanes[4] = {layer_.data(), nullptr, nullptr, nullptr};
    const int layerStride[4] = {sourceWidth * 4, 0, 0, 0};
    bandedToRgba_.convert(source->data, source->linesize, layerPlanes, layerStride);
    timing_.toRgba += since(mark);
    mark = Clock::now();

    fillOpaqueBlack(rgba_, width_, height_);
    timing_.fill += since(mark);
    mark = Clock::now();
    // compositeLayer non guarda i canali: RGBA si comporta come BGRA.
    compositeLayer(rgba_.data(), width_ * 4, width_, height_, BgraImage{layer_.data(), sourceWidth * 4, sourceWidth, sourceHeight},
                   transform);
    timing_.composite += since(mark);
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
    return toYuv(texts, props);
}

const AVFrame* TextFrameBlender::toYuv(const std::vector<BlendedText>& texts, const AVFrame* props)
{
    // I pezzi dei livelli sono BGRA e compositeLayer non guarda i canali: il fotogramma (RGBA) si scambia rosso e blu
    // per il tempo della composizione. Livelli e scritte si mettono in ordine di piano (a parità, il pezzo prima della scritta:
    // sullo stesso livello non stanno insieme).
    auto mark = Clock::now();
    bool swapped = false;
    const auto swapRedBlue = [&] {
        runBands(bandEdges(height_, bandCount(height_)), [&](int, int y0, int y1) {
            // Scambia il primo e il terzo byte di ogni pixel, una parola da 32 bit alla volta.
            std::uint32_t* p = reinterpret_cast<std::uint32_t*>(rgba_.data()) + static_cast<std::size_t>(y0) * width_;
            std::uint32_t* end = p + static_cast<std::size_t>(y1 - y0) * width_;
            for (; p < end; ++p) {
                std::uint32_t v;
                std::memcpy(&v, p, sizeof v);
                v = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                std::memcpy(p, &v, sizeof v);
            }
        });
        swapped = !swapped;
    };
    std::size_t nextText = 0;
    const auto drawText = [&](const BlendedText& text) {
        if (text.image == nullptr)
            return;
        if (swapped)
            swapRedBlue();
        blendTextOver(rgba_.data(), width_ * 4, width_, height_, *text.image, text.opacity);
    };
    for (const BlendedLayer& layer : layers_) {
        for (; nextText < texts.size() && texts[nextText].level < layer.level; ++nextText)
            drawText(texts[nextText]);
        if (layer.frame == nullptr)
            continue;
        if (!swapped)
            swapRedBlue();
        compositeLayer(rgba_.data(), width_ * 4, width_, height_,
                       BgraImage{layer.frame->pixels.data(), layer.frame->stride, layer.frame->width, layer.frame->height},
                       layer.transform);
    }
    layers_.clear();
    for (; nextText < texts.size(); ++nextText)
        drawText(texts[nextText]);
    if (swapped)
        swapRedBlue();

    timing_.overlays += since(mark);
    mark = Clock::now();
    if (av_frame_make_writable(output_.get()) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    const std::uint8_t* rgbaPlanes[4] = {rgba_.data(), nullptr, nullptr, nullptr};
    const int rgbaStride[4] = {width_ * 4, 0, 0, 0};
    if (!bandedToYuv_.prepare(width_, height_, AV_PIX_FMT_RGBA, pixelFormat_, true, false))
        throw MediaError("Conversione del fotogramma da codificare non possibile.");
    bandedToYuv_.convert(rgbaPlanes, rgbaStride, output_->data, output_->linesize);
    av_frame_copy_props(output_.get(), props);
    output_->sample_aspect_ratio = {1, 1};
    output_->pts = props->pts;
    output_->color_range = AVCOL_RANGE_MPEG;
    timing_.toYuv += since(mark);
    return output_.get();
}

}  // namespace editor::core
