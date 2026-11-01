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
    if (!reencode.includeAudio) {
        // Senza audio: niente modifiche all'audio (mix, traccia sostitutiva, tratti silenziati).
        ExportJob job(clips, outputPath, AudioEdit{}, progress, muteMethod, format, reencode);
        return job.run();
    }
    if (!audio.mix) {
        ExportJob job(clips, outputPath, audio, progress, muteMethod, format, reencode);
        return job.run();
    }
    // Audio mixato: prima si scrive il mix, poi il video con quell'audio al posto di quello dei file. Se gli spazi vuoti
    // si chiudono (per scelta o perché il nero non si sa fare) vale il mix con gli spazi chiusi.
    const PlanAnalysis plan =
        analyzePlan(clips, format, true, reencode.all, reencode.closeGaps, reencode.activeTexts(), &reencode.transitions, reencode.backend);
    if (!plan.problem.empty())
        throw ClipsNotJoinable(plan.problem);
    const MixSpec& spec = !plan.blackGaps && audio.mixClosed ? *audio.mixClosed : *audio.mix;
    // Il file temporaneo del mix: M4A per l'AAC, altrimenti Matroska (che li contiene tutti). Il file creato deve
    // poter contenere quell'audio.
    const AudioCodec mixCodec = reencode.audioCodec.value_or(AudioCodec::Aac);
    const std::string outputExtension = std::filesystem::path(pathFromUtf8(outputPath)).extension().string();
    if (!audioCodecFitsContainer(mixCodec, outputExtension))
        throw ClipsNotJoinable("Il formato " + outputExtension + " non può contenere l'audio " + audioCodecName(mixCodec)
                               + ": scegli un altro codec audio o un altro contenitore.");
    static std::atomic<int> counter{0};
    TempMixFile temp;
    temp.path = std::filesystem::temp_directory_path()
              / ("editorvideo_mix_" + std::to_string(counter.fetch_add(1)) + "_"
                 + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                 + (mixCodec == AudioCodec::Aac ? ".m4a" : ".mka"));
    constexpr double kMixShare = 0.15;
    const auto scaled = [&](double from, double to) -> ExportProgress {
        if (!progress)
            return {};
        return [&progress, from, to](double fraction) { return progress(from + (to - from) * fraction); };
    };
    const double duration = renderMixToFile(spec, utf8FromPath(temp.path), scaled(0.0, kMixShare), mixCodec, reencode.audioBitRate);
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
            analyzePlan(clips, format, audio.replacement.has_value() || audio.mix != nullptr || !reencode.includeAudio,
                        reencode.all, reencode.closeGaps, reencode.activeTexts(), &reencode.transitions, reencode.backend);
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
                    if (piece.transition >= 0) {
                        ++preview.transitionSegments;
                        preview.transitionSeconds += piece.range.length();
                    } else if (piece.text) {
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

namespace editor::core {

std::vector<ExportTransition> exportTransitionsOf(const Project& project)
{
    std::vector<ExportTransition> result;
    const Sequence& sequence = project.sequence();
    // La posizione di ogni pezzo nel piano: i pezzi senza file non ci sono (come in Project::exportPlan).
    std::vector<int> rangeOf(sequence.fragments().size(), -1);
    int next = 0;
    for (std::size_t i = 0; i < sequence.fragments().size(); ++i) {
        if (project.find(sequence.fragments()[i].clipId) != nullptr)
            rangeOf[i] = next++;
    }
    for (const TransitionPlacement& placement : project.activeTransitions()) {
        const int a = sequence.indexOf(placement.beforeFragment);
        const int b = sequence.indexOf(placement.afterFragment);
        if (a < 0 || b != a + 1 || rangeOf[static_cast<std::size_t>(a)] < 0 || rangeOf[static_cast<std::size_t>(b)] != rangeOf[static_cast<std::size_t>(a)] + 1)
            continue;
        const std::optional<TransitionJob> job = makeTransitionJob(project, placement);
        if (!job)
            continue;
        ExportTransition t;
        t.beforeRange = rangeOf[static_cast<std::size_t>(a)];
        t.before = placement.before;
        t.after = placement.after;
        t.job = *job;
        result.push_back(std::move(t));
    }
    return result;
}

}  // namespace editor::core
