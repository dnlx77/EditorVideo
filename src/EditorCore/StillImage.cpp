#include "EditorCore/StillImage.h"

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/PathUtil.h"

extern "C" {
#include <libavutil/display.h>
}

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace editor::core {

using namespace detail;

namespace {

std::string extensionOf(const std::string& utf8Path)
{
    return lowercase(utf8FromPath(pathFromUtf8(utf8Path).extension()));
}

// Come ruotare/specchiare l'immagine per mostrarla: (x, y) dell'originale va in (A*x + C*y, B*x + D*y), più lo scarto che
// la riporta dentro il fotogramma. Ogni valore è -1, 0 o 1 (solo rotazioni di 90° e specchi).
struct Orientation
{
    int a = 1, b = 0, c = 0, d = 1;
    bool swapsAxes() const { return a == 0; }
};

Orientation orientationOf(const AVFrame* frame)
{
    Orientation o;
    const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (side == nullptr || side->size < static_cast<std::size_t>(9 * sizeof(std::int32_t)))
        return o;
    const auto* m = reinterpret_cast<const std::int32_t*>(side->data);
    const auto unit = [](std::int32_t v) { return v > 0x8000 ? 1 : (v < -0x8000 ? -1 : 0); };
    const Orientation candidate{unit(m[0]), unit(m[1]), unit(m[3]), unit(m[4])};
    // Valida solo se è una vera rotazione di 90° (una riga e una colonna per asse).
    const bool diagonal = candidate.a != 0 && candidate.d != 0 && candidate.b == 0 && candidate.c == 0;
    const bool antiDiagonal = candidate.a == 0 && candidate.d == 0 && candidate.b != 0 && candidate.c != 0;
    return diagonal || antiDiagonal ? candidate : o;
}

bool decodeFirstFrame(const std::string& utf8Path, Orientation& orientation, int& srcW, int& srcH,
                                             FramePtr& decoded)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pathFromUtf8(utf8Path), ec))
        return false;
    const OpenedStream opened = openMainStream(utf8Path, AVMEDIA_TYPE_VIDEO);
    CodecContextPtr ctx(avcodec_alloc_context3(opened.codec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), opened.stream->codecpar) < 0 || avcodec_open2(ctx.get(), opened.codec, nullptr) < 0)
        return false;
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    bool flushed = false;
    for (;;) {
        const int received = avcodec_receive_frame(ctx.get(), frame.get());
        if (received == 0)
            break;
        if (received != AVERROR(EAGAIN))
            return false;  // AVERROR_EOF o errore: nessun fotogramma
        if (flushed)
            return false;
        const int read = av_read_frame(opened.fmt.get(), packet.get());
        if (read < 0) {
            avcodec_send_packet(ctx.get(), nullptr);
            flushed = true;
            continue;
        }
        const bool mine = packet->stream_index == opened.index;
        if (mine && avcodec_send_packet(ctx.get(), packet.get()) < 0) {
            av_packet_unref(packet.get());
            return false;
        }
        av_packet_unref(packet.get());
    }
    orientation = orientationOf(frame.get());
    srcW = frame->width;
    srcH = frame->height;
    decoded = std::move(frame);
    return true;
}

}  // namespace

const std::vector<std::string>& imageExtensions()
{
    static const std::vector<std::string> extensions = {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tif", ".tiff"};
    return extensions;
}

bool isImagePath(const std::string& utf8Path)
{
    const auto& known = imageExtensions();
    return std::find(known.begin(), known.end(), extensionOf(utf8Path)) != known.end();
}

std::shared_ptr<VideoFrame> loadImage(const std::string& utf8Path, int maxWidth, int maxHeight)
{
    try {
        Orientation orientation;
        int srcW = 0;
        int srcH = 0;
        FramePtr decoded;
        if (!decodeFirstFrame(utf8Path, orientation, srcW, srcH, decoded) || srcW <= 0 || srcH <= 0)
            return nullptr;

        // Prima si converte in BGRA alla misura dell'originale, poi si orienta e (se serve) si rimpicciolisce.
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(srcW) * srcH * 4);
        std::unique_ptr<SwsContext, SwsDeleter> sws(
            sws_getContext(srcW, srcH, static_cast<AVPixelFormat>(decoded->format), srcW, srcH, AV_PIX_FMT_BGRA,
                           SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws)
            return nullptr;
        std::uint8_t* dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {srcW * 4, 0, 0, 0};
        sws_scale(sws.get(), decoded->data, decoded->linesize, 0, srcH, dst, dstStride);

        const int outW = orientation.swapsAxes() ? srcH : srcW;
        const int outH = orientation.swapsAxes() ? srcW : srcH;
        auto frame = std::make_shared<VideoFrame>();
        frame->time = 0.0;
        frame->width = outW;
        frame->height = outH;
        frame->stride = outW * 4;
        frame->pixels.resize(static_cast<std::size_t>(outW) * outH * 4);
        const int offX = (orientation.a < 0 ? srcW - 1 : 0) + (orientation.c < 0 ? srcH - 1 : 0);
        const int offY = (orientation.b < 0 ? srcW - 1 : 0) + (orientation.d < 0 ? srcH - 1 : 0);
        const auto* in = reinterpret_cast<const std::uint32_t*>(bgra.data());
        auto* out = reinterpret_cast<std::uint32_t*>(frame->pixels.data());
        for (int y = 0; y < srcH; ++y) {
            for (int x = 0; x < srcW; ++x) {
                const int ox = orientation.a * x + orientation.c * y + offX;
                const int oy = orientation.b * x + orientation.d * y + offY;
                out[static_cast<std::size_t>(oy) * outW + ox] = in[static_cast<std::size_t>(y) * srcW + x];
            }
        }

        if (maxWidth > 0 && maxHeight > 0 && (outW > maxWidth || outH > maxHeight)) {
            const double scale = std::min(static_cast<double>(maxWidth) / outW, static_cast<double>(maxHeight) / outH);
            const int w = std::max(1, static_cast<int>(std::lround(outW * scale)));
            const int h = std::max(1, static_cast<int>(std::lround(outH * scale)));
            std::unique_ptr<SwsContext, SwsDeleter> shrink(sws_getContext(outW, outH, AV_PIX_FMT_BGRA, w, h,
                                                                           AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr));
            if (shrink) {
                auto small = std::make_shared<VideoFrame>();
                small->width = w;
                small->height = h;
                small->stride = w * 4;
                small->pixels.resize(static_cast<std::size_t>(w) * h * 4);
                const std::uint8_t* src[4] = {frame->pixels.data(), nullptr, nullptr, nullptr};
                const int srcStride[4] = {frame->stride, 0, 0, 0};
                std::uint8_t* smallDst[4] = {small->pixels.data(), nullptr, nullptr, nullptr};
                const int smallStride[4] = {small->stride, 0, 0, 0};
                sws_scale(shrink.get(), src, srcStride, 0, outH, smallDst, smallStride);
                return small;
            }
        }
        return frame;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::optional<ImageSize> inspectImage(const std::string& utf8Path)
{
    const auto frame = loadImage(utf8Path);
    if (!frame)
        return std::nullopt;
    return ImageSize{frame->width, frame->height};
}

}  // namespace editor::core
