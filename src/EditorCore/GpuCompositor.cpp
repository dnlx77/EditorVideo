#include "EditorCore/GpuCompositor.h"

#include "EditorCore/MediaError.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

namespace editor::core {

using detail::FramePtr;

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point from)
{
    return std::chrono::duration<double>(Clock::now() - from).count();
}

FramePtr makeFrame(AVPixelFormat format, int width, int height)
{
    FramePtr frame(av_frame_alloc());
    if (!frame)
        return nullptr;
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame.get(), 0) < 0)
        return nullptr;
    return frame;
}
}  // namespace

GpuCompositor::GpuCompositor(int width, int height, AVPixelFormat pixelFormat)
    : width_(width)
    , height_(height)
    , pixelFormat_(pixelFormat)
    , software_(av_frame_alloc())
{
}

GpuCompositor::~GpuCompositor() = default;

std::unique_ptr<GpuCompositor> GpuCompositor::create(int width, int height, AVPixelFormat pixelFormat, bool allowSoftware, std::string* why,
                                                      std::shared_ptr<GpuDevice> shared)
{
    const auto fail = [&](const std::string& reason) -> std::unique_ptr<GpuCompositor> {
        if (why)
            *why = reason;
        return nullptr;
    };
    if (pixelFormat != AV_PIX_FMT_YUV420P && pixelFormat != AV_PIX_FMT_NV12)
        return fail("Formato d'uscita non gestito dalla GPU.");
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0)
        return fail("Misura del fotogramma non gestita dalla GPU.");
    std::string error;
    std::shared_ptr<GpuDevice> device = shared ? std::move(shared) : std::shared_ptr<GpuDevice>(GpuDevice::create(allowSoftware, &error));
    if (!device)
        return fail(error);
    std::unique_ptr<GpuFrameConverter> canvas = GpuFrameConverter::create(*device, width, height);
    if (!canvas)
        return fail("Le risorse della scheda video non si riescono a creare.");
    std::unique_ptr<GpuCompositor> compositor(new GpuCompositor(width, height, pixelFormat));
    compositor->output_ = makeFrame(pixelFormat, width, height);
    if (pixelFormat == AV_PIX_FMT_YUV420P)
        compositor->nv12Out_ = makeFrame(AV_PIX_FMT_NV12, width, height);
    if (!compositor->output_ || (pixelFormat == AV_PIX_FMT_YUV420P && !compositor->nv12Out_))
        return fail("Memoria insufficiente per il fotogramma da codificare.");
    compositor->name_ = std::string("GPU: ") + device->info().adapterName;
    compositor->device_ = std::move(device);
    compositor->canvas_ = std::move(canvas);
    return compositor;
}

TextFrameBlender& GpuCompositor::cpu()
{
    if (!cpu_)
        cpu_ = std::make_unique<TextFrameBlender>(width_, height_, pixelFormat_);
    return *cpu_;
}

void GpuCompositor::followDecoderDevice(const AVFrame* frame)
{
    directChecked_ = true;
    if (device_->ownsTextureOf(frame))
        return;
    std::string why;
    std::shared_ptr<GpuDevice> adopted(GpuDevice::adopt(frame, &why));
    if (!adopted) {
        directFailed_ = true;
        timing_.directNote = "dispositivo del decoder non adottabile: " + why;
        return;
    }
    // Scritte e texture di lavoro appartengono al vecchio dispositivo: si rifanno su quello del decoder.
    textCache_.clear();
    canvas_.reset();
    std::unique_ptr<GpuFrameConverter> canvas = GpuFrameConverter::create(*adopted, width_, height_);
    if (!canvas) {
        device_ = std::move(adopted);
        fail("risorse non create sul dispositivo del decoder");
        return;
    }
    device_ = std::move(adopted);
    canvas_ = std::move(canvas);
    name_ = std::string("GPU: ") + device_->info().adapterName;
}

void GpuCompositor::fail(const std::string& reason)
{
    failed_ = true;
    failure_ = reason;
    name_ = "CPU (ripiego: " + reason + ")";
    textCache_.clear();
    canvas_.reset();
    device_.reset();
}

// Il fotogramma in NV12 in memoria: quello della scheda video si legge, yuv420p si riordina; altri formati: nullptr.
const AVFrame* GpuCompositor::nv12Of(const AVFrame* source, bool& fullRange)
{
    const AVFrame* frame = source;
    if (frame->hw_frames_ctx != nullptr) {
        if (detail::downloadHardwareFrame(software_.get(), frame) < 0)
            throw MediaError("Fotogramma della scheda video non leggibile.");
        frame = software_.get();
    }
    AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    fullRange = frame->color_range == AVCOL_RANGE_JPEG;
    detail::normalizeJpegRange(format, fullRange);
    timing_.sourceFormat = format;
    if (frame->width <= 0 || frame->height <= 0)
        return nullptr;
    if (format == AV_PIX_FMT_NV12)
        return frame;
    if (format != AV_PIX_FMT_YUV420P)
        return nullptr;
    if (!nv12In_ || nv12In_->width != frame->width || nv12In_->height != frame->height) {
        nv12In_ = makeFrame(AV_PIX_FMT_NV12, frame->width, frame->height);
        if (!nv12In_)
            throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    }
    const int w = frame->width;
    const int h = frame->height;
    for (int y = 0; y < h; ++y)
        std::memcpy(nv12In_->data[0] + static_cast<std::ptrdiff_t>(y) * nv12In_->linesize[0],
                    frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0], static_cast<std::size_t>(w));
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    for (int y = 0; y < ch; ++y) {
        const std::uint8_t* u = frame->data[1] + static_cast<std::ptrdiff_t>(y) * frame->linesize[1];
        const std::uint8_t* v = frame->data[2] + static_cast<std::ptrdiff_t>(y) * frame->linesize[2];
        std::uint8_t* out = nv12In_->data[1] + static_cast<std::ptrdiff_t>(y) * nv12In_->linesize[1];
        for (int x = 0; x < cw; ++x) {
            out[2 * x] = u[x];
            out[2 * x + 1] = v[x];
        }
    }
    return nv12In_.get();
}

bool GpuCompositor::drawText(const BlendedText& text)
{
    const TextImage* image = text.base != nullptr ? text.base : text.image;
    if (image == nullptr || image->empty())
        return true;
    std::shared_ptr<GpuTextImage> gpu;
    if (text.cacheId != 0) {
        auto found = textCache_.find(text.cacheId);
        if (found == textCache_.end())
            found = textCache_.emplace(text.cacheId, canvas_->createText(*image)).first;
        gpu = found->second;
    } else {
        gpu = canvas_->createText(*image);
    }
    if (!gpu)
        return false;
    const Transform transform = text.base != nullptr ? text.transform : Transform{};
    return canvas_->drawText(*gpu, transform, text.pivotX, text.pivotY, text.opacity);
}

// Livelli e scritte in ordine di piano (a parità, il pezzo prima della scritta), come TextFrameBlender.
bool GpuCompositor::drawOverlays(const std::vector<BlendedText>& texts)
{
    std::size_t nextText = 0;
    for (const BlendedLayer& layer : layers_) {
        for (; nextText < texts.size() && texts[nextText].level < layer.level; ++nextText)
            if (!drawText(texts[nextText]))
                return false;
        if (layer.frame == nullptr)
            continue;
        if (!canvas_->drawLayer(BgraImage{layer.frame->pixels.data(), layer.frame->stride, layer.frame->width, layer.frame->height}, layer.transform))
            return false;
    }
    for (; nextText < texts.size(); ++nextText)
        if (!drawText(texts[nextText]))
            return false;
    return true;
}

const AVFrame* GpuCompositor::compose(const AVFrame* source, const std::uint8_t* raster, int rasterStride, const Transform& transform,
                                      const std::vector<BlendedText>& texts, const AVFrame* props)
{
    const AVFrame* nv12 = nullptr;
    const AVFrame* direct = nullptr;
    bool fullRange = false;
    auto mark = Clock::now();
    if (source != nullptr) {
        // Pixel non quadrati: l'adattamento al fotogramma (blend senza trasformazione) tiene conto delle proporzioni dei pixel, la GPU no.
        const AVRational sar = source->sample_aspect_ratio;
        if (transform.isIdentity() && sar.num > 0 && sar.den > 0 && sar.num != sar.den)
            return nullptr;
        if (source->format == AV_PIX_FMT_D3D11 && source->hw_frames_ctx != nullptr && !directFailed_ && !failed_ && !directChecked_)
            followDecoderDevice(source);
        if (failed_)
            return nullptr;
        if (source->format == AV_PIX_FMT_D3D11 && source->hw_frames_ctx != nullptr && !directFailed_) {
            // Fotogramma appena decodificato dalla scheda video: si legge direttamente da lì (se non va, si passa dalla memoria).
            direct = source;
            fullRange = source->color_range == AVCOL_RANGE_JPEG;
            timing_.sourceFormat = AV_PIX_FMT_NV12;
        } else {
            nv12 = nv12Of(source, fullRange);
            if (nv12 == nullptr)
                return nullptr;  // formato non gestito: questo fotogramma lo fa la CPU
        }
    }
    if (!canvas_->clearCanvas(0.0f, 0.0f, 0.0f)) {
        fail("pulizia del fotogramma non riuscita");
        return nullptr;
    }
    if (raster != nullptr) {
        if (!canvas_->setCanvasBgra(BgraImage{raster, rasterStride, width_, height_})) {
            fail("caricamento della transizione non riuscito");
            return nullptr;
        }
    } else {
        bool drawn = false;
        if (direct != nullptr) {
            drawn = canvas_->drawD3d11Frame(direct, GpuColorSpace::bt601(fullRange), transform, &timing_.directNote);
            if (drawn) {
                ++timing_.directFrames;
            } else {
                directFailed_ = true;
                nv12 = nv12Of(source, fullRange);
                if (nv12 == nullptr)
                    return nullptr;
            }
        }
        if (!drawn && !canvas_->drawNv12(nv12, GpuColorSpace::bt601(fullRange), transform)) {
            fail("disegno del fotogramma non riuscito");
            return nullptr;
        }
    }
    timing_.composite += since(mark);
    mark = Clock::now();
    if (!drawOverlays(texts)) {
        fail("disegno di livelli e scritte non riuscito");
        return nullptr;
    }
    timing_.overlays += since(mark);
    mark = Clock::now();

    if (av_frame_make_writable(output_.get()) < 0)
        throw MediaError("Memoria insufficiente per il fotogramma da codificare.");
    const GpuColorSpace out = GpuColorSpace::bt601(false);
    AVFrame* target = pixelFormat_ == AV_PIX_FMT_NV12 ? output_.get() : nv12Out_.get();
    if (!canvas_->downloadNv12(target, out)) {
        fail("lettura del fotogramma finito non riuscita");
        return nullptr;
    }
    if (pixelFormat_ == AV_PIX_FMT_YUV420P) {
        for (int y = 0; y < height_; ++y)
            std::memcpy(output_->data[0] + static_cast<std::ptrdiff_t>(y) * output_->linesize[0],
                        nv12Out_->data[0] + static_cast<std::ptrdiff_t>(y) * nv12Out_->linesize[0], static_cast<std::size_t>(width_));
        for (int y = 0; y < height_ / 2; ++y) {
            const std::uint8_t* in = nv12Out_->data[1] + static_cast<std::ptrdiff_t>(y) * nv12Out_->linesize[1];
            std::uint8_t* u = output_->data[1] + static_cast<std::ptrdiff_t>(y) * output_->linesize[1];
            std::uint8_t* v = output_->data[2] + static_cast<std::ptrdiff_t>(y) * output_->linesize[2];
            for (int x = 0; x < width_ / 2; ++x) {
                u[x] = in[2 * x];
                v[x] = in[2 * x + 1];
            }
        }
    }
    av_frame_copy_props(output_.get(), props);
    output_->sample_aspect_ratio = {1, 1};
    output_->pts = props->pts;
    output_->color_range = AVCOL_RANGE_MPEG;
    timing_.toYuv += since(mark);

    // Le scritte che non servono più si tolgono dalla scheda video: restano solo quelle di questo fotogramma.
    if (!textCache_.empty()) {
        std::set<std::uint64_t> used;
        for (const BlendedText& text : texts)
            if (text.cacheId != 0)
                used.insert(text.cacheId);
        for (auto it = textCache_.begin(); it != textCache_.end();)
            it = used.count(it->first) != 0 ? std::next(it) : textCache_.erase(it);
    }
    layers_.clear();
    return output_.get();
}

const AVFrame* GpuCompositor::blend(const AVFrame* source, const std::vector<BlendedText>& texts)
{
    if (!failed_)
        if (const AVFrame* result = compose(source, nullptr, 0, Transform{}, texts, source))
            return result;
    cpu().setLayers(std::move(layers_));
    layers_.clear();
    return cpu().blend(source, texts);
}

const AVFrame* GpuCompositor::blendTransformed(const AVFrame* source, const Transform& transform, const std::vector<BlendedText>& texts)
{
    if (!failed_)
        if (const AVFrame* result = compose(source, nullptr, 0, transform, texts, source))
            return result;
    cpu().setLayers(std::move(layers_));
    layers_.clear();
    return cpu().blendTransformed(source, transform, texts);
}

const AVFrame* GpuCompositor::blendRaster(const std::uint8_t* bgra, int stride, int width, int height, const std::vector<BlendedText>& texts,
                                          const AVFrame* props)
{
    if (!failed_ && width == width_ && height == height_)
        if (const AVFrame* result = compose(nullptr, bgra, stride, Transform{}, texts, props))
            return result;
    cpu().setLayers(std::move(layers_));
    layers_.clear();
    return cpu().blendRaster(bgra, stride, width, height, texts, props);
}

}  // namespace editor::core
