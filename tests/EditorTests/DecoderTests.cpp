#include "EditorCore/AudioDecoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/VideoDecoder.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::sampleAudio;
using editor::tests::sampleLuma;
using editor::tests::SampleSpec;
using editor::tests::SampleDirTest;
using editor::tests::writeSampleFile;

namespace {

constexpr double kFrameDuration = 1.0 / 25.0;

class DecoderTest : public SampleDirTest<DecoderTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoDecoderTests";

    static void prepareSamples()
    {
        // 30 fotogrammi a 25 fps (1,2 s), fotogrammi chiave ogni 10.
        writeSampleFile(dir_ / "mpeg4.mkv", {.video = true, .audio = true});
        writeSampleFile(dir_ / "mpeg2.mkv", {.video = true, .audio = false, .mpeg2 = true, .width = 320, .height = 240});
        writeSampleFile(dir_ / "solo_audio.wav", {.video = false, .audio = true});
        writeSampleFile(dir_ / "solo_video.mp4", {.video = true, .audio = false});
        // 12 s, un fotogramma chiave al secondo: per la cache dei pacchetti.
        writeSampleFile(dir_ / "lungo.mkv", {.video = true, .audio = false, .frames = 300, .gopSize = 25});
        writeSampleFile(dir_ / "bframe.mkv", {.video = true, .bFrames = 2, .audio = false, .frames = 300, .gopSize = 25});
    }

protected:
    // Luminanza (Y, range TV) del pixel (x, y), ricavata dal grigio BGRA del fotogramma.
    static double lumaAt(const VideoFrame& frame, int x, int y)
    {
        const std::uint8_t* p = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride + x * 4;
        const double gray = (p[0] + p[1] + p[2]) / 3.0;
        return 16.0 + gray * 219.0 / 255.0;
    }

    // Verifica che `frame` sia il fotogramma numero `index` del file di prova (tempo e contenuto).
    static void expectFrame(const std::shared_ptr<VideoFrame>& frame, int index, int sourceWidth, int sourceHeight)
    {
        ASSERT_NE(frame, nullptr);
        EXPECT_NEAR(frame->time, index * kFrameDuration, 0.001) << "fotogramma atteso " << index;
        const int x = frame->width / 2;
        const int y = frame->height / 2;
        const int sx = x * sourceWidth / frame->width;
        const int sy = y * sourceHeight / frame->height;
        EXPECT_NEAR(lumaAt(*frame, x, y), sampleLuma(sx, sy, index), 2.5) << "contenuto del fotogramma " << index;
    }
};

TEST_F(DecoderTest, DecodesAllFramesInOrder)
{
    VideoDecoder decoder(path("mpeg4.mkv"), {.allowHardware = false});
    EXPECT_EQ(decoder.outputWidth(), 64);
    EXPECT_EQ(decoder.outputHeight(), 48);
    EXPECT_NEAR(decoder.frameRate(), 25.0, 0.01);

    auto frame = decoder.seek(0.0);
    int count = 0;
    while (frame) {
        expectFrame(frame, count, 64, 48);
        EXPECT_EQ(frame->stride, 64 * 4);
        EXPECT_EQ(frame->pixels.size(), 64u * 4u * 48u);
        ++count;
        frame = decoder.nextFrame();
    }
    EXPECT_EQ(count, 30);
}

TEST_F(DecoderTest, FrameBuffersAreReusedOnlyWhenReleased)
{
    std::vector<std::shared_ptr<VideoFrame>> held;
    std::shared_ptr<VideoFrame> survivor;
    {
        VideoDecoder decoder(path("mpeg4.mkv"), {.allowHardware = false});
        // Fotogrammi tenuti insieme (come nella coda dell'anteprima): ognuno ha il suo spazio e il suo contenuto.
        held.push_back(decoder.seek(0.0));
        held.push_back(decoder.nextFrame());
        held.push_back(decoder.nextFrame());
        for (int i = 0; i < 3; ++i)
            expectFrame(held[i], i, 64, 48);
        EXPECT_NE(held[0]->pixels.data(), held[1]->pixels.data());
        EXPECT_NE(held[1]->pixels.data(), held[2]->pixels.data());
        EXPECT_NE(held[0]->pixels.data(), held[2]->pixels.data());

        // Lasciato un fotogramma, il suo spazio serve per il successivo, con il contenuto giusto.
        const std::uint8_t* released = held[0]->pixels.data();
        held.erase(held.begin());
        survivor = decoder.nextFrame();
        expectFrame(survivor, 3, 64, 48);
        EXPECT_EQ(survivor->pixels.data(), released);
        expectFrame(held[0], 1, 64, 48);  // quelli ancora tenuti non sono stati toccati
        expectFrame(held[1], 2, 64, 48);
    }
    // Un fotogramma resta valido anche dopo la chiusura del decoder.
    expectFrame(survivor, 3, 64, 48);
    held.clear();
    survivor.reset();
}

TEST_F(DecoderTest, SeekReturnsNearestFrame)
{
    VideoDecoder decoder(path("mpeg4.mkv"), {.allowHardware = false});

    // In ordine sparso, anche all'indietro e su fotogrammi non chiave.
    const struct
    {
        double time;
        int frame;
    } cases[] = {{0.61, 15}, {0.0, 0}, {0.63, 16}, {0.41, 10}, {1.15, 29}, {0.79, 20}, {0.05, 1}, {0.39, 10}};
    for (const auto& c : cases) {
        SCOPED_TRACE(c.time);
        expectFrame(decoder.seek(c.time), c.frame, 64, 48);
    }
}

TEST_F(DecoderTest, NextFrameContinuesAfterSeek)
{
    VideoDecoder decoder(path("mpeg4.mkv"), {.allowHardware = false});
    expectFrame(decoder.seek(0.52), 13, 64, 48);
    expectFrame(decoder.nextFrame(), 14, 64, 48);
    expectFrame(decoder.nextFrame(), 15, 64, 48);
}

TEST_F(DecoderTest, SeekPastEndReturnsLastFrame)
{
    VideoDecoder decoder(path("mpeg4.mkv"), {.allowHardware = false});
    expectFrame(decoder.seek(10.0), 29, 64, 48);
    EXPECT_EQ(decoder.nextFrame(), nullptr);
}

// Indice del fotogramma che seek(t) deve restituire: il primo che inizia entro mezzo fotogramma da t.
int nearestFrame(double t)
{
    return static_cast<int>(std::ceil((t - kFrameDuration / 2) / kFrameDuration - 1e-9));
}

TEST_F(DecoderTest, CacheReadsAheadAndKeepsOnlyTheRecentPast)
{
    VideoDecoder decoder(path("lungo.mkv"), {.allowHardware = false});
    auto frame = decoder.seek(0.0);
    for (int i = 1; i <= 50; ++i)
        frame = decoder.nextFrame();
    expectFrame(frame, 50, 64, 48);
    // A 2 s: in memoria dall'inizio fino a ~10 s più avanti (qui la fine del file).
    auto [from, to] = decoder.cachedRange();
    EXPECT_NEAR(from, 0.0, 1e-6);
    EXPECT_GE(to, 11.5);

    for (int i = 51; i <= 250; ++i)
        frame = decoder.nextFrame();
    expectFrame(frame, 250, 64, 48);
    // A 10 s: dietro restano ~5 s, a gruppi interi (un fotogramma chiave al secondo).
    std::tie(from, to) = decoder.cachedRange();
    EXPECT_GE(from, 4.0 - 1e-6);
    EXPECT_LE(from, 5.0 + 1e-6);
    EXPECT_GE(to, 11.9);
}

TEST_F(DecoderTest, SeekInsideCacheDoesNotRereadTheFile)
{
    VideoDecoder decoder(path("lungo.mkv"), {.allowHardware = false});
    auto frame = decoder.seek(6.0);
    for (int i = 0; i < 50; ++i)
        frame = decoder.nextFrame();
    expectFrame(frame, 200, 64, 48);
    const double cacheStart = decoder.cachedRange().first;
    ASSERT_LE(cacheStart, 6.0);

    // Salti all'indietro e in avanti dentro la cache: fotogrammi giusti, e la cache non viene svuotata.
    for (double t : {7.29, 6.05, 6.97, 11.5, 6.5}) {
        SCOPED_TRACE(t);
        expectFrame(decoder.seek(t), nearestFrame(t), 64, 48);
        expectFrame(decoder.nextFrame(), nearestFrame(t) + 1, 64, 48);
        EXPECT_EQ(decoder.cachedRange().first, cacheStart);
    }

    // Fuori dalla cache: si rilegge il file da lì.
    expectFrame(decoder.seek(1.33), nearestFrame(1.33), 64, 48);
    EXPECT_NEAR(decoder.cachedRange().first, 1.0, 1e-6);
    expectFrame(decoder.nextFrame(), nearestFrame(1.33) + 1, 64, 48);
}

TEST_F(DecoderTest, SeeksInsideCacheWithBFramesGiveTheRightFrame)
{
    // Salti in avanti nello stesso gruppo (il decoder prosegue da dove è), a un gruppo dopo, all'indietro e sullo
    // stesso fotogramma: con i B-frame, che durante il salto non si decodificano finché non serve.
    VideoDecoder decoder(path("bframe.mkv"), {.allowHardware = false});
    auto frame = decoder.seek(4.0);
    for (int i = 0; i < 25; ++i)
        frame = decoder.nextFrame();
    expectFrame(frame, 125, 64, 48);
    for (double t : {5.2, 5.24, 5.6, 5.92, 6.1, 7.77, 4.33, 4.33, 4.36, 9.0, 8.96, 9.5}) {
        SCOPED_TRACE(t);
        expectFrame(decoder.seek(t), nearestFrame(t), 64, 48);
    }
    // Dopo un salto si continua normalmente, anche sui B-frame.
    const int at = nearestFrame(9.5);
    for (int i = 1; i <= 30; ++i)
        expectFrame(decoder.nextFrame(), at + i, 64, 48);
}

TEST_F(DecoderTest, CodecWithoutGpuSupportFallsBackToSoftware)
{
    // MPEG-4 Part 2 non è supportato da D3D11VA: anche chiedendo la GPU si deve decodificare in software.
    VideoDecoder decoder(path("mpeg4.mkv"), {.allowHardware = true});
    expectFrame(decoder.seek(0.61), 15, 64, 48);
    EXPECT_FALSE(decoder.usingHardware());
    EXPECT_EQ(decoder.decoderDescription(), "software");
}

TEST_F(DecoderTest, HardwareCanBeDisabled)
{
    VideoDecoder decoder(path("mpeg2.mkv"), {.allowHardware = false});
    auto frame = decoder.seek(0.0);
    int count = 0;
    while (frame) {
        expectFrame(frame, count++, 320, 240);
        frame = decoder.nextFrame();
    }
    EXPECT_EQ(count, 30);
    EXPECT_FALSE(decoder.usingHardware());
}

TEST_F(DecoderTest, Mpeg2DecodesWithGpuWhenAvailable)
{
    // Con una scheda video compatibile si usa D3D11VA, altrimenti il software: il risultato deve essere lo stesso.
    VideoDecoder decoder(path("mpeg2.mkv"));
    expectFrame(decoder.seek(0.61), 15, 320, 240);
    std::cout << "[ INFO     ] MPEG-2 decodificato con: " << decoder.decoderDescription()
              << (hardwareDecodingCompiledIn() ? "" : " (FFmpeg senza D3D11VA)") << '\n';

    expectFrame(decoder.seek(0.0), 0, 320, 240);
    int count = 1;
    while (auto frame = decoder.nextFrame())
        expectFrame(frame, count++, 320, 240);
    EXPECT_EQ(count, 30);
}

TEST_F(DecoderTest, FramesAreScaledToFitMaximumSize)
{
    VideoDecoder decoder(path("mpeg2.mkv"), {.allowHardware = false, .maxWidth = 160, .maxHeight = 160});
    EXPECT_EQ(decoder.outputWidth(), 160);
    EXPECT_EQ(decoder.outputHeight(), 120);
    expectFrame(decoder.seek(0.2), 5, 320, 240);

    // Riquadro dell'anteprima: si ingrandisce mantenendo le proporzioni (4:3) ...
    decoder.setOutputSize(1000, 480);
    auto frame = decoder.nextFrame();
    expectFrame(frame, 6, 320, 240);
    EXPECT_EQ(frame->width, 640);
    EXPECT_EQ(frame->height, 480);

    // ... o si rimpicciolisce, anche a metà riproduzione.
    decoder.setOutputSize(100, 100);
    frame = decoder.nextFrame();
    expectFrame(frame, 7, 320, 240);
    EXPECT_EQ(frame->width, 100);
    EXPECT_EQ(frame->height, 76);
}

TEST_F(DecoderTest, VideoDecoderRejectsAudioOnlyFile)
{
    EXPECT_THROW(VideoDecoder(path("solo_audio.wav")), MediaError);
}

TEST_F(DecoderTest, AudioDecoderReadsWholeFile)
{
    AudioDecoder decoder(path("solo_audio.wav"));
    EXPECT_EQ(decoder.sampleRate(), 48000);
    EXPECT_EQ(decoder.channels(), 2);

    long long frames = 0;
    bool first = true;
    while (auto chunk = decoder.next()) {
        if (first)
            EXPECT_NEAR(chunk->time, 0.0, 1e-9);
        first = false;
        ASSERT_EQ(chunk->samples.size() % 2, 0u);
        for (std::size_t i = 0; i < chunk->samples.size(); i += 2) {
            ASSERT_EQ(chunk->samples[i], sampleAudio(frames)) << "campione " << frames;
            ASSERT_EQ(chunk->samples[i + 1], sampleAudio(frames));
            ++frames;
        }
    }
    EXPECT_EQ(frames, 30 * 1920);
}

TEST_F(DecoderTest, AudioSeekIsSampleAccurate)
{
    for (const char* file : {"solo_audio.wav", "mpeg4.mkv"}) {
        SCOPED_TRACE(file);
        AudioDecoder decoder(path(file));
        for (double target : {0.5, 0.123, 1.0, 0.0}) {
            SCOPED_TRACE(target);
            decoder.seek(target);
            auto chunk = decoder.next();
            ASSERT_TRUE(chunk.has_value());
            EXPECT_NEAR(chunk->time, target, 1.0 / 48000);
            const auto firstSample = std::llround(target * 48000);
            EXPECT_EQ(chunk->samples[0], sampleAudio(firstSample));
            EXPECT_EQ(chunk->samples[2], sampleAudio(firstSample + 1));
        }
    }
}

TEST_F(DecoderTest, AudioDecoderRejectsVideoOnlyFile)
{
    EXPECT_THROW(AudioDecoder(path("solo_video.mp4")), MediaError);
}

// Non è un vero test: genera file dimostrativi da 10 secondi (audio muto) in %TEMP%\EditorVideoDemo
// per provare l'anteprima a mano, e un progetto (demo_progetto.evproj: due pezzi del primo file, audio tolto in un
// tratto) per --project-export-test. Si lancia con:
//   EditorTests.exe --gtest_also_run_disabled_tests --gtest_filter=*GeneraVideoDimostrativi*
TEST(DemoFiles, DISABLED_GeneraVideoDimostrativi)
{
    if (!editor::tests::sampleEncodersAvailable())
        GTEST_SKIP();
    const fs::path dir = fs::temp_directory_path() / "EditorVideoDemo";
    fs::create_directories(dir);
    SampleSpec spec{.video = true, .audio = true, .frames = 250, .gopSize = 25, .width = 640, .height = 360,
                    .silentAudio = true};
    spec.mpeg2 = true;
    writeSampleFile(dir / "demo_mpeg2_gpu.mkv", spec);
    spec.mpeg2 = false;
    writeSampleFile(dir / "demo_mpeg4_software.mkv", spec);
    spec.video = false;
    writeSampleFile(dir / "demo_solo_audio.wav", spec);

    Project project;
    const std::string video = utf8FromPath(dir / "demo_mpeg2_gpu.mkv");
    const int id = project.add(video);
    const MediaInfo info = probeFile(video);
    project.setAnalysis(id, info.keyframeTimes, info.durationSeconds);
    project.sequence().insert(-1, id, 6.0, 8.0);
    project.sequence().insert(-1, id, 1.0, 3.0);
    project.sequence().muteRange(0.5, 1.5);
    saveProjectFile(utf8FromPath(dir / "demo_progetto.evproj"), project, {});
    std::cout << "[ INFO     ] File creati in " << utf8FromPath(dir) << '\n';
}

}  // namespace
