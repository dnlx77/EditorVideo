#include "EditorCore/SegmentExportInternal.h"


namespace editor::core {

using namespace detail;

ExportCancelled::ExportCancelled()
    : MediaError("Esportazione annullata.")
{
}

using namespace segment;

std::string replacementAudioProblem(const std::string& audioPath, const std::string& outputPath, double* durationSeconds)
{
    FormatContextPtr ctx;
    try {
        ctx = openInput(audioPath);
    } catch (const std::exception& e) {
        return e.what();
    }
    const int index = findMainStream(ctx.get(), AVMEDIA_TYPE_AUDIO);
    if (durationSeconds != nullptr && index >= 0)
        *durationSeconds = std::max(0.0, durationOf(ctx.get(), ctx->streams[index]));
    return replacementProblem(ctx.get(), index, audioPath, outputPath);
}

namespace {

// Il file temporaneo del mix, cancellato alla fine.
struct TempMixFile
{
    std::filesystem::path path;
    ~TempMixFile()
    {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

}  // namespace

ExportResult exportClips(const std::vector<ClipRanges>& clips, const std::string& outputPath, const AudioEdit& audio,
                         const ExportProgress& progress, MuteMethod muteMethod, const ProjectFormat* format,
                         const ReencodeOptions& reencode)
{
    if (!audio.mix) {
        ExportJob job(clips, outputPath, audio, progress, muteMethod, format, reencode);
        return job.run();
    }
    // Audio mixato: prima si scrive il mix, poi il video con quell'audio al posto di quello dei file. Se gli spazi vuoti
    // si chiudono (per scelta o perché il nero non si sa fare) vale il mix con gli spazi chiusi.
    const PlanAnalysis plan = analyzePlan(clips, format, true, reencode.all, reencode.closeGaps, reencode.activeTexts());
    if (!plan.problem.empty())
        throw ClipsNotJoinable(plan.problem);
    const MixSpec& spec = !plan.blackGaps && audio.mixClosed ? *audio.mixClosed : *audio.mix;
    static std::atomic<int> counter{0};
    TempMixFile temp;
    temp.path = std::filesystem::temp_directory_path()
              / ("editorvideo_mix_" + std::to_string(counter.fetch_add(1)) + "_"
                 + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".m4a");
    constexpr double kMixShare = 0.15;
    const auto scaled = [&](double from, double to) -> ExportProgress {
        if (!progress)
            return {};
        return [&progress, from, to](double fraction) { return progress(from + (to - from) * fraction); };
    };
    const double duration = renderMixToFile(spec, utf8FromPath(temp.path), scaled(0.0, kMixShare));
    AudioEdit replaced;
    replaced.replacement = AudioReplacement{utf8FromPath(temp.path), 0.0, duration};
    const ExportProgress videoProgress = scaled(kMixShare, 1.0);  // ExportJob tiene un riferimento
    ExportJob job(clips, outputPath, replaced, videoProgress, muteMethod, format, reencode, &plan);
    ExportResult result = job.run();
    result.mixedAudioFiles = mixFileCount(spec);
    return result;
}

ExportPreview previewExport(const std::vector<ClipRanges>& clips, const AudioEdit& audio, const ProjectFormat* format,
                            const ReencodeOptions& reencode)
{
    ExportPreview preview;
    if (clips.empty())
        return preview;
    try {
        const PlanAnalysis plan =
            analyzePlan(clips, format, audio.replacement.has_value() || audio.mix != nullptr, reencode.all,
                        reencode.closeGaps, reencode.activeTexts());
        preview.problem = plan.problem;
        preview.impreciseCuts = plan.impreciseCuts;
        preview.closedGaps = plan.closedGaps;
        for (std::size_t c = 0; c < clips.size(); ++c) {
            for (std::size_t r = 0; r < clips[c].ranges.size(); ++r) {
                const std::vector<Piece>& pieces = plan.pieces[c][r];
                preview.effectiveRanges.push_back({pieces.front().range.start, pieces.back().range.end});
                if (const double gap = gapBefore(clips[c], r); gap > kTimeEpsilon && plan.blackGaps) {
                    ++preview.blackGaps;
                    preview.blackSeconds += gap;
                    preview.outputSeconds += gap;
                }
                if (plan.reencode[c]) {
                    ++preview.reencodedSegments;
                    preview.reencodedSeconds += clips[c].ranges[r].length();
                    preview.outputSeconds += clips[c].ranges[r].length();
                    continue;
                }
                ++preview.copiedSegments;
                for (const Piece& piece : plan.pieces[c][r]) {
                    preview.outputSeconds += piece.range.length();
                    if (piece.text) {
                        ++preview.textSegments;
                        preview.textSeconds += piece.range.length();
                    } else if (piece.reencode) {
                        ++preview.preciseCuts;
                        preview.preciseSeconds += piece.range.length();
                    } else {
                        preview.copiedSeconds += piece.range.length();
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        preview.problem = e.what();
    }
    return preview;
}

}  // namespace editor::core
