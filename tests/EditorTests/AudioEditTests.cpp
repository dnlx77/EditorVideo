#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Sequence.h"
#include "EditorCore/Waveform.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::audioTimeline;
using editor::tests::expectMonotonicPackets;
using editor::tests::expectVideoFrames;
using editor::tests::leftChannel;
using editor::tests::sampleAudio;
using editor::tests::SampleSpec;
using editor::tests::toUtf8;
using editor::tests::writeSampleFile;

namespace {

constexpr int kRate = 48000;
constexpr int kSamplesPerFrame = kRate / 25;
constexpr double kAacFrame = 1024.0 / kRate;
constexpr int kMusicFirst = 300;  // il contenuto dei file audio sostitutivi parte dal fotogramma 300 dei file di prova

// ---------------------------------------------------------------------------------------------------------------
// Modello: tratti silenziati nella sequenza

// File 1: fotogrammi chiave ogni 2 s; file 2: ogni secondo.
double snap(int clipId, double t)
{
    const double step = clipId == 1 ? 2.0 : 1.0;
    return std::floor(t / step + 1e-6) * step;
}

void expectRanges(const std::vector<TimeRange>& actual, std::initializer_list<TimeRange> expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    std::size_t i = 0;
    for (const TimeRange& e : expected) {
        EXPECT_NEAR(actual[i].start, e.start, 1e-9) << "tratto " << i;
        EXPECT_NEAR(actual[i].end, e.end, 1e-9) << "tratto " << i;
        ++i;
    }
}

TEST(AudioMute, RangeAcrossPiecesIsStoredInEachPiece)
{
    Sequence s;
    const int first = s.insert(-1, 1, 4.0, 6.0, snap);   // sequenza 0 .. 2
    const int second = s.insert(-1, 2, 0.0, 1.5, snap);  // sequenza 2 .. 3,5
    EXPECT_TRUE(s.muteRange(1.5, 2.5));
    expectRanges(s.find(first)->muted, {{5.5, 6.0}});   // tempi del file di origine
    expectRanges(s.find(second)->muted, {{0.0, 0.5}});
    expectRanges(s.mutedRanges(), {{1.5, 2.5}});         // tempi della sequenza, uniti alla giunzione

    // Lo stesso tratto di nuovo non cambia niente; uno vuoto nemmeno.
    EXPECT_FALSE(s.muteRange(1.5, 2.5));
    EXPECT_FALSE(s.muteRange(1.0, 1.0));
}

TEST(AudioMute, MutedRangesFollowTheirPiece)
{
    Sequence s;
    s.insert(-1, 1, 4.0, 6.0, snap);                     // sequenza 0 .. 2
    const int second = s.insert(-1, 2, 0.0, 1.5, snap);  // sequenza 2 .. 3,5
    s.muteRange(1.5, 2.5);
    s.move(second, 0);  // ora il secondo pezzo è in testa: 0 .. 1,5, il primo 1,5 .. 3,5
    expectRanges(s.mutedRanges(), {{0.0, 0.5}, {3.0, 3.5}});
}

TEST(AudioMute, UnmutePartOfARange)
{
    Sequence s;
    s.insert(-1, 2, 0.0, 4.0, snap);
    s.muteRange(1.0, 3.0);
    EXPECT_TRUE(s.unmuteRange(1.5, 2.0));
    expectRanges(s.mutedRanges(), {{1.0, 1.5}, {2.0, 3.0}});
    EXPECT_FALSE(s.unmuteRange(3.2, 3.8));  // lì l'audio c'è già
    EXPECT_TRUE(s.unmuteRange(0.0, 4.0));
    EXPECT_TRUE(s.mutedRanges().empty());
}

TEST(AudioMute, TouchingRangesAreMerged)
{
    Sequence s;
    s.insert(-1, 2, 0.0, 4.0, snap);
    s.muteRange(1.0, 2.0);
    s.muteRange(2.0, 2.5);
    s.muteRange(0.5, 1.2);
    expectRanges(s.mutedRanges(), {{0.5, 2.5}});
}

TEST(AudioMute, SplitShareTheRanges)
{
    Sequence s;
    const int id = s.insert(-1, 2, 0.0, 3.0, snap);
    s.muteRange(0.8, 1.3);
    const int second = s.split(1.5, snap);  // divide al fotogramma chiave di 1 s
    ASSERT_NE(second, 0);
    expectRanges(s.find(id)->muted, {{0.8, 1.0}});
    expectRanges(s.find(second)->muted, {{1.0, 1.3}});
    expectRanges(s.mutedRanges(), {{0.8, 1.3}});
}

TEST(AudioMute, CuttingAPieceTakesItsRangesAway)
{
    Sequence s;
    s.insert(-1, 2, 0.0, 4.0, snap);
    s.muteRange(0.5, 1.5);
    s.muteRange(2.5, 3.5);
    ASSERT_EQ(s.removeSourceRange(2, 1.2, 3.0, snap), Sequence::RemoveResult::Removed);
    // Restano 0 .. 1,2 e 3 .. 4, con il silenzio 0,5 .. 1,2 e 3 .. 3,5 (che nella sequenza parte da 1,2).
    ASSERT_EQ(s.fragments().size(), 2u);
    expectRanges(s.fragments()[0].muted, {{0.5, 1.2}});
    expectRanges(s.fragments()[1].muted, {{3.0, 3.5}});
    expectRanges(s.mutedRanges(), {{0.5, 1.7}});
}

TEST(AudioMute, ProjectAudioEdit)
{
    Project project;
    const int clip = project.add("video.mp4");
    project.setAnalysis(clip, {0.0, 1.0, 2.0}, 3.0);
    project.sequence().insert(-1, clip, 0.0, 3.0, project.snapper());
    project.sequence().muteRange(1.0, 2.0);
    EXPECT_TRUE(project.audioEdit().replacement == std::nullopt);
    project.setAudioReplacement(AudioReplacement{"musica.mp3", 0.5, 60.0});
    project.setAudioOffset(-2.0);
    const AudioEdit edit = project.audioEdit();
    ASSERT_TRUE(edit.replacement.has_value());
    EXPECT_EQ(edit.replacement->path, "musica.mp3");
    EXPECT_DOUBLE_EQ(edit.replacement->offset, -2.0);
    expectRanges(edit.muted, {{1.0, 2.0}});
    project.clear();
    EXPECT_TRUE(project.audioEdit().empty());
}

// ---------------------------------------------------------------------------------------------------------------
// Esportazione

class AudioExportTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoAudioTests";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!editor::tests::sampleEncodersAvailable())
            return;
        // Video da 2 s (fotogrammi chiave a 0 e 1 s) con audio PCM (MKV) e AAC (MP4).
        writeSampleFile(dir_ / "pcm.mkv", {.video = true, .audio = true, .frames = 50, .gopSize = 25});
        writeSampleFile(dir_ / "aac.mp4", {.video = true, .audio = true, .frames = 50, .gopSize = 25, .aacAudio = true});
        writeSampleFile(dir_ / "muto.mkv", {.video = true, .audio = false, .frames = 50, .gopSize = 25});
        writeSampleFile(dir_ / "mp2.mkv", {.video = true, .audio = true, .frames = 50, .gopSize = 25, .mp2Audio = true});
        // Audio sostitutivi da 3 s, con un contenuto diverso da quello dei video.
        SampleSpec music{.video = false, .audio = true, .frames = 75};
        music.firstIndex = kMusicFirst;
        writeSampleFile(dir_ / "musica.wav", music);
        music.aacAudio = true;
        writeSampleFile(dir_ / "musica.m4a", music);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void SetUp() override
    {
        if (!editor::tests::sampleEncodersAvailable())
            GTEST_SKIP() << "FFmpeg senza gli encoder necessari: impossibile generare i file di prova";
    }

    static std::string path(const std::string& name) { return toUtf8(dir_ / name); }

    static std::vector<int> allFrames()
    {
        std::vector<int> frames(50);
        for (int i = 0; i < 50; ++i)
            frames[i] = i;
        return frames;
    }

    static int maxAbs(const std::vector<std::int16_t>& samples, double from, double to)
    {
        int highest = 0;
        const auto first = static_cast<std::size_t>(from * kRate);
        const auto last = std::min(samples.size(), static_cast<std::size_t>(to * kRate));
        for (std::size_t i = first; i < last; ++i)
            highest = std::max(highest, std::abs(static_cast<int>(samples[i])));
        return highest;
    }

    static inline fs::path dir_;
};

TEST_F(AudioExportTest, MutedPcmIsSilentExactlyThere)
{
    const std::string out = path("silenzio_pcm.mkv");
    AudioEdit edit;
    edit.muted = {{0.5, 1.25}};
    const ExportResult result = exportClips({{path("pcm.mkv"), {{0.0, 2.0}}}}, out, edit);
    EXPECT_GT(result.silencedPackets, 0);
    EXPECT_EQ(result.droppedPackets, 0);
    EXPECT_NEAR(probeFile(out).durationSeconds, 2.0, 0.04);
    expectVideoFrames(out, allFrames());  // il video non cambia
    expectMonotonicPackets(out);

    const auto left = leftChannel(out);
    ASSERT_EQ(left.size(), static_cast<std::size_t>(2 * kRate));
    for (std::size_t n = 0; n < left.size(); ++n) {
        const bool muted = n >= static_cast<std::size_t>(0.5 * kRate) && n < static_cast<std::size_t>(1.25 * kRate);
        ASSERT_EQ(left[n], muted ? 0 : sampleAudio(static_cast<long long>(n))) << "campione " << n;
    }
}

TEST_F(AudioExportTest, MutedAacKeepsTheAudioContinuous)
{
    const std::string out = path("silenzio_aac.mp4");
    AudioEdit edit;
    edit.muted = {{0.5, 1.2}};
    const ExportResult result = exportClips({{path("aac.mp4"), {{0.0, 2.0}}}}, out, edit);
    // I pacchetti del tratto restano, con il contenuto sostituito da un blocco di silenzio AAC.
    EXPECT_GE(result.silencedPackets, static_cast<int>(0.7 / kAacFrame) - 1);
    EXPECT_EQ(result.droppedPackets, 0);
    EXPECT_NEAR(probeFile(out).durationSeconds, 2.0, 0.05);
    expectVideoFrames(out, allFrames());
    expectMonotonicPackets(out);

    // Nessun buco nei tempi dell'audio: ogni blocco comincia dove finisce il precedente.
    const auto timeline = audioTimeline(out);
    EXPECT_FALSE(timeline.overlaps);
    for (std::size_t i = 1; i < timeline.starts.size(); ++i)
        EXPECT_LT(timeline.starts[i] - timeline.starts[i - 1], kAacFrame + 0.002) << "buco prima del blocco " << i;
    const auto original = audioTimeline(path("aac.mp4"));
    EXPECT_NEAR(timeline.totalSeconds, original.totalSeconds, 2 * kAacFrame);

    // Dentro il tratto silenzio (lasciando un blocco ai bordi), fuori il suono di prima.
    const auto left = leftChannel(out);
    EXPECT_LT(maxAbs(left, 0.5 + 2 * kAacFrame, 1.2 - 2 * kAacFrame), 50);
    EXPECT_GT(maxAbs(left, 0.1, 0.4), 3000);
    EXPECT_GT(maxAbs(left, 1.3, 1.9), 3000);
}

TEST_F(AudioExportTest, MutedMpegAudioUsesSilentFrames)
{
    // MP2 (come l'MP3): il silenzio è lo stesso fotogramma con i dati azzerati, senza bisogno di un encoder.
    const std::string out = path("silenzio_mp2.mkv");
    AudioEdit edit;
    edit.muted = {{0.5, 1.2}};
    const ExportResult result = exportClips({{path("mp2.mkv"), {{0.0, 2.0}}}}, out, edit);
    constexpr double kMp2Frame = 1152.0 / kRate;
    EXPECT_GE(result.silencedPackets, static_cast<int>(0.7 / kMp2Frame) - 1);
    EXPECT_EQ(result.droppedPackets, 0);
    expectMonotonicPackets(out);

    const auto timeline = audioTimeline(out);
    EXPECT_FALSE(timeline.overlaps);
    for (std::size_t i = 1; i < timeline.starts.size(); ++i)
        EXPECT_LT(timeline.starts[i] - timeline.starts[i - 1], kMp2Frame + 0.002) << "buco prima del blocco " << i;
    EXPECT_NEAR(timeline.totalSeconds, audioTimeline(path("mp2.mkv")).totalSeconds, 2 * kMp2Frame);

    const auto left = leftChannel(out);
    EXPECT_LT(maxAbs(left, 0.5 + 2 * kMp2Frame, 1.2 - 2 * kMp2Frame), 50);
    EXPECT_GT(maxAbs(left, 0.1, 0.4), 3000);
    EXPECT_GT(maxAbs(left, 1.3, 1.9), 3000);
}

TEST_F(AudioExportTest, OmittingPacketsLeavesAGap)
{
    // L'alternativa scartata: togliere i pacchetti lascia un buco nel flusso audio.
    const std::string out = path("omessi_aac.mp4");
    AudioEdit edit;
    edit.muted = {{0.5, 1.2}};
    const ExportResult result = exportClips({{path("aac.mp4"), {{0.0, 2.0}}}}, out, edit, {}, MuteMethod::Omit);
    EXPECT_GT(result.droppedPackets, 0);
    EXPECT_EQ(result.silencedPackets, 0);
    const auto timeline = audioTimeline(out);
    const auto original = audioTimeline(path("aac.mp4"));
    EXPECT_LT(timeline.totalSeconds, original.totalSeconds - 0.5);
    expectMonotonicPackets(out);
}

TEST_F(AudioExportTest, MutedRangeAcrossAJoin)
{
    // Montaggio di due pezzi dello stesso file (1 .. 2 s, poi 0 .. 1 s) con il silenzio a cavallo della giunzione.
    Project project;
    const std::string file = path("pcm.mkv");
    const MediaInfo info = probeFile(file);
    const int id = project.add(file);
    project.setAnalysis(id, info.keyframeTimes, info.durationSeconds);
    project.sequence().insert(-1, id, 1.0, 2.0, project.snapper());
    project.sequence().insert(-1, id, 0.0, 1.0, project.snapper());
    project.sequence().muteRange(0.8, 1.2);

    const std::string out = path("silenzio_giunzione.mkv");
    exportClips(project.exportPlan(), out, project.audioEdit());
    const auto left = leftChannel(out);
    ASSERT_EQ(left.size(), static_cast<std::size_t>(2 * kRate));
    for (std::size_t n = 0; n < left.size(); ++n) {
        const bool firstPiece = n < static_cast<std::size_t>(kRate);
        const long long source = firstPiece ? static_cast<long long>(n) + kRate : static_cast<long long>(n) - kRate;
        const bool muted = n >= static_cast<std::size_t>(0.8 * kRate) && n < static_cast<std::size_t>(1.2 * kRate);
        ASSERT_EQ(left[n], muted ? 0 : sampleAudio(source)) << "campione " << n;
    }
}

TEST_F(AudioExportTest, ReplacementAacIsCopiedAtItsOffset)
{
    const std::string out = path("sostituito.mkv");
    AudioEdit edit;
    edit.replacement = AudioReplacement{path("musica.m4a"), 0.5, 3.0};
    const ExportResult result = exportClips({{path("pcm.mkv"), {{0.0, 2.0}}}}, out, edit);
    EXPECT_EQ(result.streamCount, 2);

    const MediaInfo info = probeFile(out);
    ASSERT_EQ(info.streams.size(), 2u);
    EXPECT_EQ(info.streams[0].codec, "mpeg4");
    EXPECT_EQ(info.streams[1].codec, "aac");  // copiato, non ricodificato
    EXPECT_NEAR(info.durationSeconds, 2.0, 0.05);
    expectVideoFrames(out, allFrames());
    expectMonotonicPackets(out);

    // L'audio comincia a 0,5 s e si ferma con il video, senza buchi.
    const auto timeline = audioTimeline(out);
    ASSERT_FALSE(timeline.starts.empty());
    EXPECT_NEAR(timeline.starts.front(), 0.5, 0.03);
    EXPECT_FALSE(timeline.overlaps);
    EXPECT_NEAR(timeline.starts.front() + timeline.totalSeconds, 2.0, 2 * kAacFrame);
    for (std::size_t i = 1; i < timeline.starts.size(); ++i)
        EXPECT_LT(timeline.starts[i] - timeline.starts[i - 1], kAacFrame + 0.002) << "buco prima del blocco " << i;
}

TEST_F(AudioExportTest, ReplacementWithNegativeOffsetSkipsItsStart)
{
    // WAV in MKV: PCM, quindi si controlla al campione. Con offset -1 s il file creato parte dal secondo 1 dell'audio.
    const std::string out = path("sostituito_pcm.mkv");
    AudioEdit edit;
    edit.replacement = AudioReplacement{path("musica.wav"), -1.0, 3.0};
    edit.muted = {{1.5, 1.75}};  // il silenzio vale anche per la traccia sostitutiva
    exportClips({{path("pcm.mkv"), {{0.0, 2.0}}}}, out, edit);

    const auto left = leftChannel(out);
    ASSERT_EQ(left.size(), static_cast<std::size_t>(2 * kRate));
    const long long first = static_cast<long long>(kMusicFirst) * kSamplesPerFrame + kRate;
    for (std::size_t n = 0; n < left.size(); ++n) {
        const bool muted = n >= static_cast<std::size_t>(1.5 * kRate) && n < static_cast<std::size_t>(1.75 * kRate);
        ASSERT_EQ(left[n], muted ? 0 : sampleAudio(first + static_cast<long long>(n))) << "campione " << n;
    }
}

TEST_F(AudioExportTest, ReplacementGivesAudioToASilentVideo)
{
    const std::string out = path("con_musica.mkv");
    AudioEdit edit;
    edit.replacement = AudioReplacement{path("musica.wav"), 0.0, 3.0};
    exportClips({{path("muto.mkv"), {{0.0, 2.0}}}}, out, edit);
    const MediaInfo info = probeFile(out);
    ASSERT_EQ(info.streams.size(), 2u);
    EXPECT_EQ(info.streams[1].kind, StreamKind::Audio);
    EXPECT_EQ(leftChannel(out).size(), static_cast<std::size_t>(2 * kRate));  // tagliato alla fine del video
}

TEST_F(AudioExportTest, IncompatibleReplacementIsRefused)
{
    // PCM (WAV) in un MP4: senza ricodifica non si può.
    const std::string problem = replacementAudioProblem(path("musica.wav"), path("x.mp4"));
    EXPECT_NE(problem.find("non può stare in un file MP4"), std::string::npos) << problem;
    EXPECT_TRUE(replacementAudioProblem(path("musica.wav"), path("x.mkv")).empty());
    EXPECT_TRUE(replacementAudioProblem(path("musica.m4a"), path("x.mp4")).empty());
    EXPECT_FALSE(replacementAudioProblem(path("muto.mkv"), path("x.mkv")).empty());  // niente audio

    AudioEdit edit;
    edit.replacement = AudioReplacement{path("musica.wav"), 0.0, 3.0};
    const std::string out = path("rifiutato.mp4");
    try {
        exportClips({{path("aac.mp4"), {{0.0, 2.0}}}}, out, edit);
        ADD_FAILURE() << "l'esportazione doveva essere rifiutata";
    } catch (const MediaError& e) {
        EXPECT_NE(std::string(e.what()).find("musica.wav"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(dir_ / "rifiutato.mp4"));
}

TEST_F(AudioExportTest, WaveformOfTheTestFiles)
{
    const Waveform wave = computeWaveform(path("pcm.mkv"));
    EXPECT_NEAR(wave.duration(), 2.0, 0.02);
    EXPECT_GT(wave.peak(0.0, 2.0), 0.2);
    EXPECT_TRUE(computeWaveform(path("muto.mkv")).empty());

    // Il silenzio esportato si vede nella forma d'onda.
    const std::string out = path("onda.mkv");
    AudioEdit edit;
    edit.muted = {{0.5, 1.5}};
    exportClips({{path("pcm.mkv"), {{0.0, 2.0}}}}, out, edit);
    const Waveform muted = computeWaveform(out);
    EXPECT_EQ(muted.peak(0.52, 1.48), 0.0);
    EXPECT_GT(muted.peak(0.0, 0.48), 0.2);
}

}  // namespace
