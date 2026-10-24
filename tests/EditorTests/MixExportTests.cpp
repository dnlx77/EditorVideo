#include "EditorCore/AudioDecoder.h"
#include "EditorCore/AudioMixer.h"
#include "EditorCore/MixExport.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::SampleDirTest;
using editor::tests::SampleSpec;
using editor::tests::writeSampleFile;

namespace {

constexpr int kRate = kMixSampleRate;

// WAV stereo a 16 bit con una sinusoide: un segnale che l'AAC riproduce quasi uguale, così l'errore si misura.
void writeSine(const fs::path& path, double seconds, double frequency, double amplitude)
{
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds * kRate);
    const std::uint32_t bytes = frames * 4;
    std::ofstream out(path, std::ios::binary);
    const auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    put32(36 + bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(2);
    put32(kRate);
    put32(kRate * 4);
    put16(4);
    put16(16);
    out.write("data", 4);
    put32(bytes);
    for (std::uint32_t n = 0; n < frames; ++n) {
        const auto v = static_cast<std::int16_t>(amplitude * 32767.0 * std::sin(2.0 * std::numbers::pi * frequency * n / kRate));
        put16(static_cast<std::uint16_t>(v));
        put16(static_cast<std::uint16_t>(v));
    }
}

std::vector<std::int16_t> decodeAll(const std::string& file)
{
    AudioDecoder decoder(file, kRate);
    std::vector<std::int16_t> all;
    while (auto chunk = decoder.next()) {
        if (decoder.channels() == 1) {
            for (std::int16_t v : chunk->samples) {
                all.push_back(v);
                all.push_back(v);
            }
        } else {
            all.insert(all.end(), chunk->samples.begin(), chunk->samples.end());
        }
    }
    return all;
}

std::vector<std::int16_t> mixOffline(const MixSpec& spec, double seconds)
{
    AudioMixer mixer(spec.segments, spec.gains, spec.envelopes);
    std::vector<std::int16_t> all, chunk;
    const long long total = std::llround(seconds * kRate);
    for (long long frame = 0; frame < total; frame += 4096) {
        mixer.render(frame, static_cast<std::size_t>(std::min<long long>(4096, total - frame)), chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    return all;
}

// Errore quadratico medio fra i due segnali (canale sinistro) diviso per la potenza del riferimento, e il ritardo (in
// campioni) che li fa somigliare di più, cercato in [-maxLag, maxLag].
struct Comparison
{
    double error = 0.0;
    int bestLag = 0;
};

Comparison compare(const std::vector<std::int16_t>& reference, const std::vector<std::int16_t>& actual, long long from,
                   long long to, int maxLag = 3000)
{
    const auto left = [](const std::vector<std::int16_t>& v, long long i) {
        return i >= 0 && static_cast<std::size_t>(i) * 2 < v.size() ? static_cast<double>(v[static_cast<std::size_t>(i) * 2]) : 0.0;
    };
    const auto errorAt = [&](int lag) {
        double diff = 0.0, power = 0.0;
        for (long long i = from; i < to; ++i) {
            const double r = left(reference, i), a = left(actual, i + lag);
            diff += (r - a) * (r - a);
            power += r * r;
        }
        return power > 0.0 ? diff / power : diff;
    };
    Comparison best{errorAt(0), 0};
    for (int lag = -maxLag; lag <= maxLag; lag += 1) {
        if (const double e = errorAt(lag); e < best.error) {
            best.error = e;
            best.bestLag = lag;
        }
    }
    return best;
}

class MixExportTest : public SampleDirTest<MixExportTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoMixExportTests";

    static void prepareSamples()
    {
        writeSine(dir_ / "la.wav", 3.0, 440.0, 0.5);
        writeSine(dir_ / "mi.wav", 3.0, 660.0, 0.4);
        // Video senza rumore nell'audio: l'audio di A1 è silenzio, così si sente solo quello che si aggiunge.
        writeSampleFile(dir_ / "video.mkv", {.video = true, .audio = true, .frames = 100, .gopSize = 25, .silentAudio = true});
        writeSampleFile(dir_ / "voce.mkv", {.video = true, .audio = true, .frames = 100, .gopSize = 25});
    }

protected:
    static AudioClip clipOf(const char* name, double start, double end, double position)
    {
        AudioClip c;
        c.path = path(name);
        c.start = start;
        c.end = end;
        c.position = position;
        c.sourceDuration = 3.0;
        return c;
    }

    static Project montage(const char* video)
    {
        Project project;
        const int clip = project.add(path(video));
        project.setAnalysis(clip, {0.0, 1.0, 2.0, 3.0}, 4.0);
        project.sequence().insert(-1, clip, 0.0, 3.0);
        return project;
    }
};

}  // namespace

TEST_F(MixExportTest, RenderedFileIsTheSameMixAsThePreview)
{
    Project project = montage("video.mkv");
    AudioTracks& audio = project.audioTracks();
    const int a2 = audio.addTrack();
    const int a3 = audio.addTrack();
    ASSERT_NE(audio.add(a2, clipOf("la.wav", 0.0, 2.0, 0.5)), 0);
    ASSERT_NE(audio.add(a3, clipOf("mi.wav", 0.5, 2.5, 1.0)), 0);
    audio.setLevel(a3, -6.0);
    audio.setEnvelope(a2, {{0.5, 0.0}, {1.0, -20.0}, {2.0, -20.0}, {2.5, 0.0}});  // si abbassa solo una parte
    const MixSpec spec = mixSpecOf(project);
    EXPECT_EQ(mixFileCount(spec), 3);

    const std::string out = path("mix.m4a");
    const double seconds = renderMixToFile(spec, out);
    EXPECT_NEAR(seconds, 3.0, 1e-3);

    const auto expected = mixOffline(spec, 3.0);
    const auto actual = decodeAll(out);
    EXPECT_NEAR(static_cast<double>(actual.size()) / 2 / kRate, 3.0, 0.05);  // la durata del file
    const Comparison c = compare(expected, actual, kRate / 10, kRate * 5 / 2);
    EXPECT_EQ(c.bestLag, 0) << "l'audio dev'essere a tempo con la sequenza";
    EXPECT_LT(c.error, 0.02) << "AAC a 256 kb/s: l'errore dev'essere piccolo";
}

TEST_F(MixExportTest, RenderCanBeCancelledAndLeavesNoFile)
{
    Project project = montage("video.mkv");
    const int a2 = project.audioTracks().addTrack();
    ASSERT_NE(project.audioTracks().add(a2, clipOf("la.wav", 0.0, 2.0, 0.0)), 0);
    const std::string out = path("annullato.m4a");
    EXPECT_THROW(renderMixToFile(mixSpecOf(project), out, [](double f) { return f < 0.3; }), ExportCancelled);
    EXPECT_FALSE(fs::exists(editor::core::pathFromUtf8(out)));
}

TEST_F(MixExportTest, ExportedMontageHasTheVideoAndTheMixedAudioInTime)
{
    Project project = montage("video.mkv");
    AudioTracks& audio = project.audioTracks();
    const int a2 = audio.addTrack();
    ASSERT_NE(audio.add(a2, clipOf("la.wav", 0.0, 2.0, 0.5)), 0);
    audio.setEnvelope(a2, {{0.5, -60.0}, {1.5, 0.0}});  // entra piano
    const AudioEdit edit = project.audioEdit(GapExport::Black);
    ASSERT_TRUE(edit.mix);

    const std::string out = path("montaggio.mkv");
    double lastProgress = 0.0;
    bool decreasing = false;
    const ExportResult result =
        exportClips(project.exportPlan(GapExport::Black), out, edit, [&](double f) {
            decreasing = decreasing || f < lastProgress;
            lastProgress = f;
            return true;
        });
    EXPECT_EQ(result.mixedAudioFiles, 2);
    EXPECT_FALSE(decreasing);
    EXPECT_DOUBLE_EQ(lastProgress, 1.0);

    std::vector<int> frames(75);
    for (int i = 0; i < 75; ++i)
        frames[static_cast<std::size_t>(i)] = i;
    editor::tests::expectVideoFrames(out, frames);

    const auto actual = decodeAll(out);
    EXPECT_NEAR(static_cast<double>(actual.size()) / 2 / kRate, 3.0, 0.06);
    const Comparison c = compare(mixOffline(mixSpecOf(project), 3.0), actual, kRate / 2, kRate * 5 / 2);
    EXPECT_EQ(c.bestLag, 0);
    EXPECT_LT(c.error, 0.03);
    // Prima di 0,5 s non c'è niente (A1 è silenzio), fra 0,5 e 1,5 la dissolvenza sale.
    long long loud = 0;
    for (long long i = 0; i < kRate / 2 - 2000; ++i)
        loud += std::abs(actual[static_cast<std::size_t>(i) * 2]);
    EXPECT_LT(loud / (kRate / 2 - 2000), 40);
    const auto peak = [&](double from, double to) {
        int p = 0;
        for (long long i = std::llround(from * kRate); i < std::llround(to * kRate); ++i)
            p = std::max<int>(p, std::abs(actual[static_cast<std::size_t>(i) * 2]));
        return p;
    };
    EXPECT_LT(peak(0.5, 0.6), peak(1.4, 1.5) / 4);
    EXPECT_GT(peak(1.6, 2.4), 12000);
}

TEST_F(MixExportTest, LevelOfTheVideoAudioAloneIsAppliedToo)
{
    // Nessuna clip sulle tracce aggiunte, ma A1 a -12 dB: l'audio non si può copiare com'è.
    Project project = montage("voce.mkv");
    project.audioTracks().setLevel(AudioTracks::kOriginalTrack, -12.0);
    const AudioEdit edit = project.audioEdit(GapExport::Black);
    ASSERT_TRUE(edit.mix);
    const std::string out = path("a1.mkv");
    const ExportResult result = exportClips(project.exportPlan(GapExport::Black), out, edit);
    EXPECT_EQ(result.mixedAudioFiles, 1);
    const auto actual = decodeAll(out);
    const Comparison c = compare(mixOffline(mixSpecOf(project), 3.0), actual, kRate / 2, kRate * 5 / 2);
    EXPECT_EQ(c.bestLag, 0);

    // Senza modifiche al mix l'audio resta quello copiato.
    Project plain = montage("voce.mkv");
    EXPECT_FALSE(plain.audioEdit(GapExport::Black).mix);
    EXPECT_EQ(exportClips(plain.exportPlan(GapExport::Black), path("copia.mkv"), plain.audioEdit(GapExport::Black)).mixedAudioFiles, 0);
}

TEST_F(MixExportTest, ClosedGapsMoveThePiecesButNotTheAddedClips)
{
    Project project;
    const int clip = project.add(path("video.mkv"));
    project.setAnalysis(clip, {0.0, 1.0, 2.0, 3.0}, 4.0);
    const int first = project.sequence().insert(-1, clip, 0.0, 1.0);
    const int second = project.sequence().insert(-1, clip, 1.0, 2.0);
    ASSERT_TRUE(project.sequence().moveTo(second, 2.0));  // spazio vuoto di 1 s
    (void)first;
    const int a2 = project.audioTracks().addTrack();
    ASSERT_NE(project.audioTracks().add(a2, clipOf("la.wav", 0.0, 1.0, 2.5)), 0);
    project.audioTracks().setLevel(AudioTracks::kOriginalTrack, -3.0);
    const AudioEdit edit = project.audioEdit(GapExport::Black);
    ASSERT_TRUE(edit.mix);
    ASSERT_TRUE(edit.mixClosed);
    EXPECT_DOUBLE_EQ(edit.mix->length, 3.0);
    EXPECT_DOUBLE_EQ(edit.mixClosed->length, 2.0);
    EXPECT_DOUBLE_EQ(edit.mix->segments[1].position, 2.0);  // A1, secondo pezzo
    bool found = false;
    for (const MixSegment& s : edit.mixClosed->segments) {
        if (s.trackId == AudioTracks::kOriginalTrack && s.fileStart == 1.0) {
            EXPECT_DOUBLE_EQ(s.position, 1.0);  // senza lo spazio il pezzo è a 1 s
            found = true;
        }
        if (s.trackId == a2)
            EXPECT_DOUBLE_EQ(s.position, 2.5);
    }
    EXPECT_TRUE(found);
}
