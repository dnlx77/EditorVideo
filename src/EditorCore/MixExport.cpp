#include "EditorCore/MixExport.h"

#include "EditorCore/AudioMixer.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

namespace editor::core {

using namespace detail;

namespace {

constexpr int kBlockFrames = 4096;
constexpr int kMixBitRate = 256000;

struct OutputDeleter
{
    void operator()(AVFormatContext* ctx) const
    {
        if (ctx == nullptr)
            return;
        if (ctx->pb != nullptr && (ctx->oformat->flags & AVFMT_NOFILE) == 0)
            avio_closep(&ctx->pb);
        avformat_free_context(ctx);
    }
};

}  // namespace

int mixFileCount(const MixSpec& spec)
{
    std::set<std::string> paths;
    for (const MixSegment& s : spec.segments)
        paths.insert(s.path);
    return static_cast<int>(paths.size());
}

double renderMixToFile(const MixSpec& spec, const std::string& path, const ExportProgress& progress, AudioCodec codec,
                       int bitRate)
{
    AudioMixer mixer(spec.segments, spec.gains);
    const double seconds = std::max(spec.length, mixer.endTime());
    const long long totalFrames = std::max<long long>(1, std::llround(seconds * kMixSampleRate));

    AudioEncoder encoder(AudioEncoderSettings{kMixSampleRate, kMixChannels, bitRate > 0 ? bitRate : kMixBitRate, codec});

    AVFormatContext* raw = nullptr;
    if (avformat_alloc_output_context2(&raw, nullptr, nullptr, path.c_str()) < 0 || raw == nullptr)
        throw MediaError("Dall'estensione non si capisce in che formato salvare l'audio: " + path);
    std::unique_ptr<AVFormatContext, OutputDeleter> output(raw);
    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    if (stream == nullptr)
        throw MediaError("Memoria insufficiente per l'audio mixato.");
    encoder.copyParameters(stream->codecpar);
    stream->time_base = {encoder.timeBase().num, encoder.timeBase().den};

    bool started = false;
    const auto remove = [&] {
        output.reset();
        std::error_code ec;
        std::filesystem::remove(pathFromUtf8(path), ec);
    };
    try {
        if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
            if (const int ret = avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE); ret < 0)
                throw MediaError("Impossibile creare il file dell'audio mixato (" + ffmpegError(ret) + "): " + path);
        }
        if (const int ret = avformat_write_header(output.get(), nullptr); ret < 0)
            throw MediaError("Il file dell'audio mixato non si può iniziare (" + ffmpegError(ret) + ").");
        started = true;

        PacketPtr copy(av_packet_alloc());
        FramePtr frame(av_frame_alloc());
        if (!copy || !frame)
            throw MediaError("Memoria insufficiente per l'audio mixato.");
        const AVRational encoderBase{encoder.timeBase().num, encoder.timeBase().den};
        const PacketSink sink = [&](AVPacket* packet) {
            av_packet_ref(copy.get(), packet);  // il pacchetto è dell'encoder: se ne scrive una copia
            av_packet_rescale_ts(copy.get(), encoderBase, stream->time_base);
            copy->stream_index = stream->index;
            const int ret = av_interleaved_write_frame(output.get(), copy.get());
            av_packet_unref(copy.get());
            if (ret < 0)
                throw MediaError("Errore nel scrivere l'audio mixato (" + ffmpegError(ret) + ").");
        };

        std::vector<std::int16_t> block;
        for (long long done = 0; done < totalFrames;) {
            if (progress && !progress(static_cast<double>(done) / static_cast<double>(totalFrames)))
                throw ExportCancelled();
            const int count = static_cast<int>(std::min<long long>(kBlockFrames, totalFrames - done));
            mixer.render(done, static_cast<std::size_t>(count), block);
            av_frame_unref(frame.get());
            frame->nb_samples = count;
            frame->format = AV_SAMPLE_FMT_S16;
            frame->sample_rate = kMixSampleRate;
            av_channel_layout_default(&frame->ch_layout, kMixChannels);
            if (av_frame_get_buffer(frame.get(), 0) < 0)
                throw MediaError("Memoria insufficiente per l'audio mixato.");
            std::copy(block.begin(), block.end(), reinterpret_cast<std::int16_t*>(frame->data[0]));
            encoder.encode(frame.get(), sink);
            done += count;
        }
        encoder.finish(sink);
        if (const int ret = av_write_trailer(output.get()); ret < 0)
            throw MediaError("Errore nel completare il file dell'audio mixato (" + ffmpegError(ret) + ").");
        if (progress)
            progress(1.0);
    } catch (...) {
        if (started || output)
            remove();
        throw;
    }
    return static_cast<double>(totalFrames) / kMixSampleRate;
}

}  // namespace editor::core
