#include "EditorCore/AudioMixer.h"
#include "EditorCore/Project.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace editor::core;
using editor::tests::sampleAudio;
using editor::tests::SampleDirTest;
using editor::tests::SampleSpec;
using editor::tests::writeSampleFile;

namespace {

constexpr long long kRate = kMixSampleRate;

class AudioMixerTest : public SampleDirTest<AudioMixerTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoAudioMixerTests";

    static void prepareSamples()
    {
        // 2 s di audio di prova (50 fotogrammi a 25 fps); "b" ha il contenuto che riparte da 2 s.
        writeSampleFile(dir_ / "a.wav", {.video = false, .audio = true, .frames = 50});
        writeSampleFile(dir_ / "b.wav", {.video = false, .audio = true, .frames = 50, .firstIndex = 50});
        // 1 s a 44,1 kHz: la frequenza del mix è un'altra.
        writeSampleFile(dir_ / "cd.wav", {.video = false, .audio = true, .frames = 25, .sampleRate = 44100});
        writeSampleFile(dir_ / "video.mkv", {.video = true, .audio = true, .frames = 50, .gopSize = 25});
    }

protected:
    static MixSegment segment(const char* name, double fileStart, double fileEnd, double position, int track = 0)
    {
        MixSegment s;
        s.path = path(name);
        s.fileStart = fileStart;
        s.fileEnd = fileEnd;
        s.position = position;
        s.trackId = track;
        return s;
    }

    // Tutto il mix da 0 a `seconds`, a blocchi di `block` fotogrammi (come lo chiede il lettore); sinistra e destra
    // devono essere uguali (i file di prova sono mono in stereo).
    static std::vector<std::int16_t> renderAll(AudioMixer& mixer, double seconds, std::size_t block = 1024)
    {
        std::vector<std::int16_t> all;
        const auto total = static_cast<long long>(seconds * kRate);
        std::vector<std::int16_t> chunk;
        for (long long frame = 0; frame < total; frame += static_cast<long long>(block)) {
            mixer.render(frame, static_cast<std::size_t>(std::min<long long>(block, total - frame)), chunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
        return all;
    }
};

}  // namespace

TEST_F(AudioMixerTest, OneSegmentLandsOnTheExactSample)
{
    // [0,5 s - 1,5 s) del file a 0,25 s della sequenza.
    AudioMixer mixer({segment("a.wav", 0.5, 1.5, 0.25)}, {{0, 1.0f}});
    const auto out = renderAll(mixer, 2.0, 777);  // blocchi che non cadono sui bordi
    ASSERT_EQ(out.size(), static_cast<std::size_t>(2 * kRate * 2));
    for (long long f = 0; f < 2 * kRate; ++f) {
        std::int16_t expected = 0;
        if (f >= kRate / 4 && f < kRate / 4 + kRate)
            expected = sampleAudio(kRate / 2 + (f - kRate / 4));
        ASSERT_EQ(out[f * 2], expected) << "fotogramma " << f;
        ASSERT_EQ(out[f * 2 + 1], expected) << "fotogramma " << f;
    }
}

TEST_F(AudioMixerTest, SeekingAroundGivesTheSameSamples)
{
    AudioMixer mixer({segment("a.wav", 0.0, 2.0, 0.0)}, {{0, 1.0f}});
    std::vector<std::int16_t> out;
    mixer.render(60000, 500, out);  // salto avanti
    for (long long i = 0; i < 500; ++i)
        ASSERT_EQ(out[i * 2], sampleAudio(60000 + i));
    mixer.render(10000, 300, out);  // e indietro
    for (long long i = 0; i < 300; ++i)
        ASSERT_EQ(out[i * 2], sampleAudio(10000 + i));
    mixer.render(10300, 300, out);  // di seguito
    for (long long i = 0; i < 300; ++i)
        ASSERT_EQ(out[i * 2], sampleAudio(10300 + i));
}

TEST_F(AudioMixerTest, PiecesFollowEachOtherWithoutAGap)
{
    // Due pezzi uno dopo l'altro (A1) e una musica sotto, che continua per tutto il tempo.
    AudioMixer mixer({segment("a.wav", 0.0, 1.0, 0.0), segment("b.wav", 0.0, 1.0, 1.0),
                      segment("a.wav", 0.0, 2.0, 0.0, 1)},
                     {{0, 1.0f}, {1, 0.5f}});
    const auto out = renderAll(mixer, 2.0);
    for (long long f = 0; f < 2 * kRate; ++f) {
        // A1: a.wav 0-1 s, poi b.wav (contenuto da 2 s: indice + 2 s). La musica: a.wav intero.
        const long long own = f < kRate ? sampleAudio(f) : sampleAudio(2 * kRate + (f - kRate));
        const long long music = sampleAudio(f);
        const long long expected = std::lround(own + music * 0.5);
        ASSERT_NEAR(out[f * 2], expected, 1) << "fotogramma " << f;
    }
}

TEST_F(AudioMixerTest, MutedRangesAreSilentExactlyThere)
{
    MixSegment s = segment("a.wav", 0.0, 2.0, 0.0);
    s.muted = {{0.5, 0.75}, {1.5, 1.6}};
    AudioMixer mixer({s}, {{0, 1.0f}});
    const auto out = renderAll(mixer, 2.0, 1000);
    for (long long f = 0; f < 2 * kRate; ++f) {
        const bool muted = (f >= kRate / 2 && f < kRate * 3 / 4) || (f >= kRate * 3 / 2 && f < kRate * 8 / 5);
        ASSERT_EQ(out[f * 2], muted ? 0 : sampleAudio(f)) << "fotogramma " << f;
    }
}

TEST_F(AudioMixerTest, TrackLevelAndSilencedTracks)
{
    AudioMixer mixer({segment("a.wav", 0.0, 1.0, 0.0), segment("b.wav", 0.0, 1.0, 0.0, 1)}, {{0, 1.0f}, {1, 0.0f}});
    auto out = renderAll(mixer, 1.0);
    for (long long f = 0; f < kRate; ++f)
        ASSERT_EQ(out[f * 2], sampleAudio(f)) << "la traccia a livello 0 non si sente";

    AudioMixer louder({segment("a.wav", 0.0, 1.0, 0.0)}, {{0, 0.5f}});
    out = renderAll(louder, 1.0);
    for (long long f = 0; f < kRate; ++f)
        ASSERT_NEAR(out[f * 2], sampleAudio(f) * 0.5, 1) << "fotogramma " << f;
}

TEST_F(AudioMixerTest, LevelChangesWithARampNotAJump)
{
    AudioMixer mixer({segment("a.wav", 0.0, 2.0, 0.0)}, {{0, 1.0f}});
    std::vector<std::int16_t> out;
    mixer.render(0, 1024, out);
    mixer.setGain(0, 0.0f);  // da un thread qualsiasi, mentre si suona
    mixer.render(1024, 1024, out);
    // Nel blocco il livello scende da 1 a 0: il primo campione è quasi intero, l'ultimo quasi zero, e non sale mai.
    double previous = 2.0;
    for (std::size_t i = 0; i < 1024; ++i) {
        const double sample = sampleAudio(1024 + static_cast<long long>(i));
        if (std::abs(sample) < 2000)
            continue;
        const double ratio = out[i * 2] / sample;
        ASSERT_LE(ratio, previous + 0.01);
        previous = ratio;
    }
    EXPECT_GT(std::abs(out[0]), 0);
    mixer.render(2048, 1024, out);
    for (std::size_t i = 0; i < 1024; ++i)
        ASSERT_EQ(out[i * 2], 0);
}

TEST_F(AudioMixerTest, LimiterNeverExceedsFullScale)
{
    EXPECT_FLOAT_EQ(AudioMixer::limit(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(AudioMixer::limit(-0.9f), -0.9f);
    EXPECT_LE(AudioMixer::limit(3.0f), 1.0f);
    EXPECT_GT(AudioMixer::limit(3.0f), 0.95f);
    EXPECT_FLOAT_EQ(AudioMixer::limit(-3.0f), -AudioMixer::limit(3.0f));
    // Sale sempre, senza scalini.
    float previous = 0.0f;
    for (float v = 0.0f; v < 4.0f; v += 0.01f) {
        EXPECT_GE(AudioMixer::limit(v), previous);
        previous = AudioMixer::limit(v);
    }

    // Sei copie sovrapposte di un segnale di +-0,3 arrivano a +-1,8: non devono andare oltre il fondo scala.
    std::vector<MixSegment> loud;
    for (int i = 0; i < 6; ++i)
        loud.push_back(segment("a.wav", 0.0, 1.0, 0.0, i));
    std::map<int, float> gains;
    for (int i = 0; i < 6; ++i)
        gains[i] = 1.0f;
    AudioMixer mixer(loud, gains);
    const auto out = renderAll(mixer, 1.0);
    for (std::int16_t v : out) {
        ASSERT_GE(v, -32768);
        ASSERT_LE(v, 32767);
    }
    EXPECT_GT(std::abs(out[1000 * 2]), 0);
}

TEST_F(AudioMixerTest, OtherSampleRatesAreConvertedToTheMixRate)
{
    // 1 s a 44,1 kHz: nel mix dura 1 s a 48 kHz e poi c'è silenzio.
    AudioMixer mixer({segment("cd.wav", 0.0, 1.0, 0.5)}, {{0, 1.0f}});
    const auto out = renderAll(mixer, 2.0);
    auto energy = [&](double from, double to) {
        double sum = 0;
        for (long long f = static_cast<long long>(from * kRate); f < static_cast<long long>(to * kRate); ++f)
            sum += std::abs(out[f * 2]);
        return sum / ((to - from) * kRate);
    };
    EXPECT_EQ(energy(0.0, 0.49), 0.0);
    EXPECT_GT(energy(0.55, 1.45), 1000.0);
    EXPECT_EQ(energy(1.55, 2.0), 0.0);
}

TEST_F(AudioMixerTest, ProjectGivesPiecesAndTracks)
{
    Project project;
    const int clip = project.add(path("video.mkv"));
    project.setAnalysis(clip, {0.0, 1.0}, 2.0);
    const int other = project.add(path("b.wav"));
    project.setAnalysis(other, {}, 2.0);
    project.sequence().insert(-1, clip, 0.5, 1.5);
    project.sequence().insert(-1, other, 0.0, 1.0);
    project.sequence().muteRange(0.25, 0.5);
    AudioTracks& audio = project.audioTracks();
    const int a2 = audio.addTrack();
    AudioClip music;
    music.path = path("a.wav");
    music.start = 0.5;
    music.end = 1.5;
    music.position = 0.75;
    music.sourceDuration = 2.0;
    ASSERT_NE(audio.add(a2, music), 0);
    audio.setLevel(a2, -6.0);
    audio.setMuted(AudioTracks::kOriginalTrack, true);

    const auto segments = mixSegmentsOf(project);
    ASSERT_EQ(segments.size(), 3u);
    EXPECT_EQ(segments[0].trackId, AudioTracks::kOriginalTrack);
    EXPECT_DOUBLE_EQ(segments[0].position, 0.0);
    EXPECT_DOUBLE_EQ(segments[0].fileStart, 0.5);
    ASSERT_EQ(segments[0].muted.size(), 1u);
    EXPECT_DOUBLE_EQ(segments[0].muted[0].start, 0.75);  // 0,25-0,5 della sequenza = 0,75-1,0 nel file
    EXPECT_EQ(segments[1].trackId, a2);
    EXPECT_DOUBLE_EQ(segments[1].position, 0.75);
    EXPECT_EQ(segments[2].trackId, AudioTracks::kOriginalTrack);
    EXPECT_DOUBLE_EQ(segments[2].position, 1.0);

    const auto gains = trackGainsOf(audio);
    EXPECT_FLOAT_EQ(gains.at(AudioTracks::kOriginalTrack), 0.0f);  // A1 muta
    EXPECT_NEAR(gains.at(a2), std::pow(10.0, -6.0 / 20.0), 1e-6);
}

TEST_F(AudioMixerTest, FileWithoutAudioOrMissingFileIsSilence)
{
    MixSegment missing = segment("non_esiste.wav", 0.0, 1.0, 0.0, 1);
    AudioMixer mixer({segment("a.wav", 0.0, 1.0, 0.0), missing}, {{0, 1.0f}, {1, 1.0f}});
    const auto out = renderAll(mixer, 1.0);
    for (long long f = 0; f < kRate; ++f)
        ASSERT_EQ(out[f * 2], sampleAudio(f));
}
