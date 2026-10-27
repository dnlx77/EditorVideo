#include "EditorCore/TransitionRender.h"

#include "EditorCore/Transitions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

namespace editor::core {

// Un lato della transizione: il decoder del suo file e il fotogramma a cui è arrivato.
struct TransitionRenderer::Side
{
    std::string path;  // file da cui si legge il video (anche il proxy)
    std::unique_ptr<VideoDecoder> decoder;
    std::shared_ptr<VideoFrame> current;  // l'ultimo fotogramma non dopo il tempo chiesto (o quello cercato)
    std::shared_ptr<VideoFrame> next;     // già letto ma ancora nel futuro
    bool ended = false;                   // il file è finito

    void close()
    {
        decoder.reset();
        current.reset();
        next.reset();
        ended = false;
        path.clear();
    }

    // Il fotogramma da mostrare al tempo `time` del file. Se il tempo avanza di poco si legge in avanti, altrimenti si
    // riposiziona il decoder.
    std::shared_ptr<VideoFrame> frameAt(double time)
    {
        const double frameDuration = decoder->frameRate() > 0 ? 1.0 / decoder->frameRate() : 0.04;
        constexpr double kMaxAhead = 1.0;
        constexpr double kEpsilon = 1e-6;
        if (!current || time < current->time - frameDuration || time > current->time + kMaxAhead) {
            current = decoder->seek(time);
            next.reset();
            ended = false;
            return current;
        }
        for (;;) {
            if (!next && !ended) {
                next = decoder->nextFrame();
                ended = next == nullptr;
            }
            if (!next || next->time > time + kEpsilon)
                break;
            current = std::move(next);
            next.reset();
        }
        return current;
    }
};

TransitionRenderer::TransitionRenderer()
    : from_(std::make_unique<Side>())
    , to_(std::make_unique<Side>())
{
}

TransitionRenderer::~TransitionRenderer() = default;

void TransitionRenderer::setBox(int width, int height)
{
    width = std::max(2, width);
    height = std::max(2, height);
    if (width == boxWidth_ && height == boxHeight_)
        return;
    boxWidth_ = width;
    boxHeight_ = height;
    reset();
}

void TransitionRenderer::setVideoSource(std::function<std::string(const std::string& path)> source)
{
    videoSource_ = std::move(source);
    reset();
}

void TransitionRenderer::setHardwareAllowed(bool allowed)
{
    if (allowed != hardware_) {
        hardware_ = allowed;
        reset();
    }
}

void TransitionRenderer::reset()
{
    from_->close();
    to_->close();
}

namespace {

// Mette il fotogramma al centro di un riquadro nero della misura `box` (BGRA, alfa 255).
std::shared_ptr<VideoFrame> onCanvas(const VideoFrame& frame, int boxWidth, int boxHeight)
{
    auto canvas = std::make_shared<VideoFrame>();
    canvas->time = frame.time;
    canvas->width = boxWidth;
    canvas->height = boxHeight;
    canvas->stride = boxWidth * 4;
    canvas->pixels.assign(static_cast<std::size_t>(canvas->stride) * boxHeight, 0);
    for (std::size_t i = 3; i < canvas->pixels.size(); i += 4)
        canvas->pixels[i] = 255;
    const int copyWidth = std::min(frame.width, boxWidth);
    const int copyHeight = std::min(frame.height, boxHeight);
    const int x0 = (boxWidth - copyWidth) / 2;
    const int y0 = (boxHeight - copyHeight) / 2;
    const int sx0 = (frame.width - copyWidth) / 2;
    const int sy0 = (frame.height - copyHeight) / 2;
    for (int y = 0; y < copyHeight; ++y) {
        std::memcpy(canvas->pixels.data() + static_cast<std::size_t>(y0 + y) * canvas->stride + static_cast<std::size_t>(x0) * 4,
                    frame.pixels.data() + static_cast<std::size_t>(sy0 + y) * frame.stride + static_cast<std::size_t>(sx0) * 4,
                    static_cast<std::size_t>(copyWidth) * 4);
    }
    return canvas;
}

}  // namespace

std::shared_ptr<VideoFrame> TransitionRenderer::render(const Project& project, const TransitionPlacement& placement,
                                                       double sequenceTime)
{
    const TransitionClip* transition = project.transitions().find(placement.id);
    const Transition* effect = transition != nullptr ? findTransition(transition->type) : nullptr;
    const Sequence& sequence = project.sequence();
    const Fragment* a = sequence.find(placement.beforeFragment);
    const Fragment* b = sequence.find(placement.afterFragment);
    if (effect == nullptr || a == nullptr || b == nullptr)
        return nullptr;
    const Clip* clipA = project.find(a->clipId);
    const Clip* clipB = project.find(b->clipId);
    if (clipA == nullptr || clipB == nullptr)
        return nullptr;

    const double t = std::clamp(sequenceTime, placement.start(), placement.end());
    // Tempi nei file: il pezzo che finisce continua oltre la sua fine, quello che inizia parte prima del suo inizio.
    double timeA = a->start + (t - sequence.startOf(sequence.indexOf(a->id)));
    double timeB = b->start + (t - sequence.startOf(sequence.indexOf(b->id)));
    if (clipA->duration > 0)
        timeA = std::min(timeA, clipA->duration);
    timeA = std::max(timeA, 0.0);
    timeB = std::max(timeB, 0.0);
    if (clipB->duration > 0)
        timeB = std::min(timeB, clipB->duration);

    const auto open = [&](Side& side, const Clip& clip) {
        const std::string path = videoSource_ ? videoSource_(clip.path) : clip.path;
        if (side.decoder != nullptr && side.path == path)
            return true;
        side.close();
        try {
            VideoDecoder::Options options;
            options.allowHardware = hardware_;
            options.maxWidth = boxWidth_;
            options.maxHeight = boxHeight_;
            options.readAhead = false;
            side.decoder = std::make_unique<VideoDecoder>(path, options);
            side.path = path;
            return true;
        } catch (const std::exception&) {
            side.close();
            return false;
        }
    };
    if (!open(*from_, *clipA) || !open(*to_, *clipB))
        return nullptr;
    std::shared_ptr<VideoFrame> frameA;
    std::shared_ptr<VideoFrame> frameB;
    try {
        frameA = from_->frameAt(timeA);
        frameB = to_->frameAt(timeB);
    } catch (const std::exception&) {
        reset();
        return nullptr;
    }
    if (frameA == nullptr || frameB == nullptr)
        return nullptr;

    // Se un lato ha un fotogramma più piccolo del riquadro (o di altre proporzioni) sta al centro, con i bordi neri.
    std::shared_ptr<VideoFrame> out = onCanvas(*frameA, boxWidth_, boxHeight_);
    const std::shared_ptr<VideoFrame> second = onCanvas(*frameB, boxWidth_, boxHeight_);
    const double duration = placement.duration();
    const double progress = duration > 0 ? (t - placement.start()) / duration : 1.0;
    effect->blend({out->pixels.data(), out->width, out->height, out->stride},
                  {second->pixels.data(), second->width, second->height, second->stride}, progress, transition->params,
                  {out->pixels.data(), out->stride});
    out->time = t;
    return out;
}

}  // namespace editor::core
