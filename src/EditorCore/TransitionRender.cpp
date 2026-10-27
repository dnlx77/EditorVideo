#include "EditorCore/TransitionRender.h"

#include "EditorCore/Transitions.h"

#include <algorithm>
#include <chrono>
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
    long long seeks = 0;                  // quante volte si è riposizionato il decoder

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
            ++seeks;
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

std::optional<TransitionJob> makeTransitionJob(const Project& project, const TransitionPlacement& placement)
{
    const TransitionClip* transition = project.transitions().find(placement.id);
    const Sequence& sequence = project.sequence();
    const Fragment* a = sequence.find(placement.beforeFragment);
    const Fragment* b = sequence.find(placement.afterFragment);
    if (transition == nullptr || a == nullptr || b == nullptr)
        return std::nullopt;
    const Clip* clipA = project.find(a->clipId);
    const Clip* clipB = project.find(b->clipId);
    if (clipA == nullptr || clipB == nullptr)
        return std::nullopt;
    TransitionJob job;
    job.typeId = transition->type;
    job.params = transition->params;
    job.pathA = clipA->path;
    job.pathB = clipB->path;
    job.durationA = clipA->duration;
    job.durationB = clipB->duration;
    job.fragmentStartA = a->start;
    job.fragmentStartB = b->start;
    job.sequenceStartA = sequence.startOf(sequence.indexOf(a->id));
    job.sequenceStartB = sequence.startOf(sequence.indexOf(b->id));
    job.start = placement.start();
    job.end = placement.end();
    return job;
}

std::shared_ptr<VideoFrame> TransitionRenderer::render(const Project& project, const TransitionPlacement& placement,
                                                       double sequenceTime)
{
    const std::optional<TransitionJob> job = makeTransitionJob(project, placement);
    return job ? render(*job, sequenceTime) : nullptr;
}

std::shared_ptr<VideoFrame> TransitionRenderer::render(const TransitionJob& job, double sequenceTime)
{
    const Transition* effect = findTransition(job.typeId);
    if (effect == nullptr)
        return nullptr;

    const double t = std::clamp(sequenceTime, job.start, job.end);
    // Tempi nei file: il pezzo che finisce continua oltre la sua fine, quello che inizia parte prima del suo inizio.
    double timeA = job.fragmentStartA + (t - job.sequenceStartA);
    double timeB = job.fragmentStartB + (t - job.sequenceStartB);
    if (job.durationA > 0)
        timeA = std::min(timeA, job.durationA);
    timeA = std::max(timeA, 0.0);
    timeB = std::max(timeB, 0.0);
    if (job.durationB > 0)
        timeB = std::min(timeB, job.durationB);

    const auto open = [&](Side& side, const std::string& original) {
        const std::string path = videoSource_ ? videoSource_(original) : original;
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
    if (!open(*from_, job.pathA) || !open(*to_, job.pathB))
        return nullptr;
    std::shared_ptr<VideoFrame> frameA;
    std::shared_ptr<VideoFrame> frameB;
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto since = [&](std::chrono::steady_clock::time_point from) { return std::chrono::duration<double>(now() - from).count(); };
    try {
        auto begin = now();
        frameA = from_->frameAt(timeA);
        timing_.decodeFrom = since(begin);
        begin = now();
        frameB = to_->frameAt(timeB);
        timing_.decodeTo = since(begin);
        timing_.seeks = from_->seeks + to_->seeks;
        timing_.hardwareFrom = from_->decoder->usingHardware();
        timing_.hardwareTo = to_->decoder->usingHardware();
    } catch (const std::exception&) {
        reset();
        return nullptr;
    }
    if (frameA == nullptr || frameB == nullptr)
        return nullptr;

    // Se un lato ha un fotogramma più piccolo del riquadro (o di altre proporzioni) sta al centro, con i bordi neri.
    const auto blendBegin = now();
    std::shared_ptr<VideoFrame> out = onCanvas(*frameA, boxWidth_, boxHeight_);
    const std::shared_ptr<VideoFrame> second = onCanvas(*frameB, boxWidth_, boxHeight_);
    const double duration = job.end - job.start;
    const double progress = duration > 0 ? (t - job.start) / duration : 1.0;
    effect->blend({out->pixels.data(), out->width, out->height, out->stride},
                  {second->pixels.data(), second->width, second->height, second->stride}, progress, job.params,
                  {out->pixels.data(), out->stride});
    out->time = t;
    timing_.blend = since(blendBegin);
    return out;
}

}  // namespace editor::core
