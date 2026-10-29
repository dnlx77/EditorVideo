#include "EditorCore/Project.h"
#include "EditorCore/TransitionPlayback.h"
#include "EditorCore/TransitionRender.h"
#include "EditorCore/Transitions.h"
#include "EditorCore/VideoDecoder.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace editor::core;
using namespace editor::tests;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;

class TransitionRenderTest : public SampleDirTest<TransitionRenderTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoTransitionRenderTests";

    // Due file di 4 s (100 fotogrammi) con contenuto diverso: il secondo parte dal fotogramma 50.
    static void prepareSamples()
    {
        writeSampleFile(dir_ / "a.mkv", {.video = true, .audio = false, .frames = 100, .gopSize = 10, .width = kWidth, .height = kHeight});
        writeSampleFile(dir_ / "b.mkv",
                        {.video = true, .audio = false, .frames = 100, .gopSize = 10, .width = kWidth, .height = kHeight, .firstIndex = 50});
    }

protected:
    // A = a.mkv [0, 2] e B = b.mkv [1, 3], attaccati: la giunzione è a 2 s della sequenza. Transizione di 1 s: da 1,5 a
    // 2,5 si vede A oltre la sua fine (secondi 1,5..2,5 del file) e B prima del suo inizio (0,5..1,5 del file).
    struct Setup
    {
        Project project;
        int transition = 0;
        TransitionPlacement placement;
    };

    static std::unique_ptr<Setup> makeSetup(const std::string& type = "dissolvenza", double duration = 1.0)
    {
        auto setup = std::make_unique<Setup>();
        const int a = setup->project.add(path("a.mkv"));
        const int b = setup->project.add(path("b.mkv"));
        setup->project.setAnalysis(a, {0.0}, 4.0);
        setup->project.setAnalysis(b, {0.0}, 4.0);
        const int fa = setup->project.sequence().insert(-1, a, 0.0, 2.0);
        setup->project.sequence().insert(-1, b, 1.0, 3.0);
        setup->transition = setup->project.addTransition(fa, type, duration);
        setup->placement = setup->project.transitionPlacement(setup->transition).value_or(TransitionPlacement{});
        return setup;
    }

    // Il fotogramma del file al tempo dato, decodificato da solo (software, misura originale).
    static std::shared_ptr<VideoFrame> decode(const std::string& file, double time)
    {
        VideoDecoder decoder(path(file), {.allowHardware = false, .maxWidth = kWidth, .maxHeight = kHeight, .readAhead = false});
        return decoder.seek(time);
    }

    static std::unique_ptr<TransitionRenderer> makeRenderer(int width = kWidth, int height = kHeight)
    {
        auto renderer = std::make_unique<TransitionRenderer>();
        renderer->setHardwareAllowed(false);
        renderer->setBox(width, height);
        return renderer;
    }

    static RgbaView viewOf(const VideoFrame& frame) { return {frame.pixels.data(), frame.width, frame.height, frame.stride}; }
};

}  // namespace

TEST_F(TransitionRenderTest, StartsAsTheFirstPieceAndEndsAsTheSecond)
{
    auto setup = makeSetup();
    ASSERT_NE(setup->transition, 0);
    EXPECT_NEAR(setup->placement.start(), 1.5, 1e-9);
    EXPECT_NEAR(setup->placement.end(), 2.5, 1e-9);
    auto renderer = makeRenderer();
    const auto first = renderer->render(setup->project, setup->placement, 1.52);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->width, kWidth);
    EXPECT_EQ(first->height, kHeight);
    // Un fotogramma dopo l'inizio è ancora quasi solo A (4%): niente di B oltre l'arrotondamento.
    const auto a = decode("a.mkv", 1.52);
    const auto b = decode("b.mkv", 0.52);
    ASSERT_TRUE(a && b);
    int maxDiffA = 0;
    for (std::size_t i = 0; i < first->pixels.size(); ++i)
        maxDiffA = std::max(maxDiffA, std::abs(static_cast<int>(first->pixels[i]) - static_cast<int>(a->pixels[i])));
    int maxDiffB = 0;
    for (std::size_t i = 0; i < first->pixels.size(); ++i)
        maxDiffB = std::max(maxDiffB, std::abs(static_cast<int>(first->pixels[i]) - static_cast<int>(b->pixels[i])));
    EXPECT_LE(maxDiffA, 12);
    EXPECT_GT(maxDiffB, 12);  // i due fotogrammi sono diversi: la prova distingue i lati
    const auto last = renderer->render(setup->project, setup->placement, 2.48);
    ASSERT_NE(last, nullptr);
    const auto lastB = decode("b.mkv", 1.48);
    const auto lastA = decode("a.mkv", 2.48);
    int diffToB = 0;
    int diffToA = 0;
    for (std::size_t i = 0; i < last->pixels.size(); ++i) {
        diffToB = std::max(diffToB, std::abs(static_cast<int>(last->pixels[i]) - static_cast<int>(lastB->pixels[i])));
        diffToA = std::max(diffToA, std::abs(static_cast<int>(last->pixels[i]) - static_cast<int>(lastA->pixels[i])));
    }
    EXPECT_LE(diffToB, 12);
    EXPECT_GT(diffToA, 12);
}

TEST_F(TransitionRenderTest, HalfwayIsTheCrossfadeOfTheRightFrames)
{
    auto setup = makeSetup();
    auto renderer = makeRenderer();
    const auto out = renderer->render(setup->project, setup->placement, 2.0);
    ASSERT_NE(out, nullptr);
    const auto a = decode("a.mkv", 2.0);  // A oltre la sua fine (finiva a 2 s del file)
    const auto b = decode("b.mkv", 1.0);  // B al suo primo fotogramma
    ASSERT_TRUE(a && b);
    VideoFrame expected = *a;
    findTransition("dissolvenza")->blend(viewOf(*a), viewOf(*b), 0.5, {}, {expected.pixels.data(), expected.stride});
    EXPECT_EQ(out->pixels, expected.pixels);
}

TEST_F(TransitionRenderTest, ReadingForwardGivesTheSameFramesAsJumping)
{
    auto setup = makeSetup("dissolvenza-nero");
    auto sequential = makeRenderer();
    for (int k = 0; k < 24; ++k) {
        const double t = 1.52 + 0.04 * k;
        const auto stepped = sequential->render(setup->project, setup->placement, t);
        auto jumped = makeRenderer();
        const auto fresh = jumped->render(setup->project, setup->placement, t);
        ASSERT_TRUE(stepped && fresh) << k;
        EXPECT_EQ(stepped->pixels, fresh->pixels) << "t=" << t;
    }
    // E all'indietro (riposiziona i decoder).
    const auto back = sequential->render(setup->project, setup->placement, 1.6);
    auto fresh = makeRenderer();
    EXPECT_EQ(back->pixels, fresh->render(setup->project, setup->placement, 1.6)->pixels);
}

TEST_F(TransitionRenderTest, TimesOutsideTheTransitionAreClamped)
{
    auto setup = makeSetup();
    auto renderer = makeRenderer();
    const auto before = renderer->render(setup->project, setup->placement, 0.2);
    const auto atStart = makeRenderer()->render(setup->project, setup->placement, 1.5);
    ASSERT_TRUE(before && atStart);
    EXPECT_EQ(before->pixels, atStart->pixels);
    const auto after = renderer->render(setup->project, setup->placement, 3.9);
    const auto atEnd = makeRenderer()->render(setup->project, setup->placement, 2.5);
    ASSERT_TRUE(after && atEnd);
    EXPECT_EQ(after->pixels, atEnd->pixels);
}

TEST_F(TransitionRenderTest, SmallerPicturesSitInTheMiddleOfTheBox)
{
    auto setup = makeSetup();
    // Riquadro più largo dei file: le immagini (64x48) stanno al centro, ai lati nero.
    auto renderer = makeRenderer(160, kHeight);
    const auto out = renderer->render(setup->project, setup->placement, 1.5);  // all'inizio: solo A
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->width, 160);
    const auto pixel = [&](const VideoFrame& f, int x, int y) { return f.pixels.data() + static_cast<std::size_t>(y) * f.stride + x * 4; };
    for (int x : {0, 47, 112, 159}) {
        EXPECT_EQ(pixel(*out, x, 10)[0], 0) << x;
        EXPECT_EQ(pixel(*out, x, 10)[1], 0) << x;
        EXPECT_EQ(pixel(*out, x, 10)[2], 0) << x;
        EXPECT_EQ(pixel(*out, x, 10)[3], 255) << x;
    }
    // Nel mezzo c'è A, riga per riga.
    const auto a = decode("a.mkv", 1.5);
    ASSERT_TRUE(a);
    for (int x : {0, 10, 31, 63})
        EXPECT_EQ(pixel(*out, 48 + x, 20)[0], pixel(*a, x, 20)[0]) << x;
}

TEST_F(TransitionRenderTest, FailsCleanlyWithoutTransitionOrFiles)
{
    auto setup = makeSetup();
    auto renderer = makeRenderer();
    TransitionPlacement missing = setup->placement;
    missing.id = 999;
    EXPECT_EQ(renderer->render(setup->project, missing, 2.0), nullptr);
    TransitionPlacement noPieces = setup->placement;
    noPieces.beforeFragment = 999;
    EXPECT_EQ(renderer->render(setup->project, noPieces, 2.0), nullptr);
    // Un file che non si apre.
    Project broken;
    const int a = broken.add(path("non-esiste.mkv"));
    const int b = broken.add(path("b.mkv"));
    broken.setAnalysis(a, {0.0}, 4.0);
    broken.setAnalysis(b, {0.0}, 4.0);
    const int fa = broken.sequence().insert(-1, a, 0.0, 2.0);
    broken.sequence().insert(-1, b, 1.0, 3.0);
    const int id = broken.addTransition(fa, "dissolvenza", 1.0);
    ASSERT_NE(id, 0);
    EXPECT_EQ(renderer->render(broken, *broken.transitionPlacement(id), 2.0), nullptr);
}

TEST_F(TransitionRenderTest, PlaybackThreadGivesTheSameFramesAsTheRenderer)
{
    auto setup = makeSetup("scorrimento");
    const auto job = makeTransitionJob(setup->project, setup->placement);
    ASSERT_TRUE(job.has_value());
    TransitionPlayback playback;
    playback.setHardwareAllowed(false);
    playback.setBox(kWidth, kHeight);
    for (const double t : {1.52, 1.56, 2.0, 2.4}) {
        const auto threaded = playback.render(*job, t);
        const auto direct = makeRenderer()->render(setup->project, setup->placement, t);
        ASSERT_TRUE(threaded && direct) << t;
        EXPECT_EQ(threaded->pixels, direct->pixels) << t;
    }
    const auto latest = playback.latest();
    ASSERT_NE(latest.frame, nullptr);
    EXPECT_NEAR(latest.time, 2.4, 1e-9);
    EXPECT_GT(playback.averageRenderSeconds(), 0.0);
}

TEST_F(TransitionRenderTest, PlaybackThreadKeepsOnlyTheLastRequest)
{
    auto setup = makeSetup();
    const auto job = makeTransitionJob(setup->project, setup->placement);
    ASSERT_TRUE(job.has_value());
    TransitionPlayback playback;
    playback.setHardwareAllowed(false);
    playback.setBox(kWidth, kHeight);
    // Tante richieste di fila: alla fine arriva comunque l'ultima.
    unsigned long long last = 0;
    for (int k = 0; k < 20; ++k)
        last = playback.request(*job, 1.52 + 0.04 * k);
    const auto frame = playback.render(*job, 2.0);
    ASSERT_NE(frame, nullptr);
    const auto latest = playback.latest();
    EXPECT_GT(latest.serial, last);
    EXPECT_NEAR(latest.time, 2.0, 1e-9);
    // Una richiesta fatta con una misura diversa si applica da quella dopo.
    playback.setBox(32, 24);
    const auto small = playback.render(*job, 2.0);
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(small->width, 32);
    EXPECT_EQ(small->height, 24);
}

namespace {

// Aspetta (al massimo 5 s) un fotogramma del flusso per `time`, che abbia raggiunto almeno `minTime` (avanti) o al più
// `maxTime` (indietro).
std::shared_ptr<VideoFrame> waitFrame(TransitionPlayback& playback, double time, bool forward, double tolerance)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto frame = playback.takeFrame(time);
        if (frame && (forward ? frame->time >= time - tolerance : frame->time <= time + tolerance))
            return frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return nullptr;
}

}  // namespace

TEST_F(TransitionRenderTest, StreamPreparesTheFramesInOrderAhead)
{
    auto setup = makeSetup("dissolvenza");
    const auto job = makeTransitionJob(setup->project, setup->placement);
    ASSERT_TRUE(job.has_value());
    TransitionPlayback playback;
    playback.setHardwareAllowed(false);
    playback.setBox(kWidth, kHeight);
    playback.startStream(*job, 1.52, 0.04, +1);  // sui tempi dei fotogrammi: scelta del fotogramma senza ambiguità
    std::shared_ptr<VideoFrame> previous;
    for (int k = 0; k <= 24; ++k) {
        const double t = 1.52 + 0.04 * k;
        const auto frame = waitFrame(playback, t, true, 1e-6);
        ASSERT_NE(frame, nullptr) << k;
        EXPECT_NEAR(frame->time, std::min(t, 2.5), 1e-6) << k;
        EXPECT_NE(frame, previous) << k;
        // Lo stesso fotogramma torna finché non arriva il tempo del successivo.
        EXPECT_EQ(playback.takeFrame(t + 0.01), frame) << k;
        const auto direct = makeRenderer()->render(setup->project, setup->placement, frame->time);
        ASSERT_NE(direct, nullptr);
        EXPECT_EQ(frame->pixels, direct->pixels) << k;
        previous = frame;
    }
}

TEST_F(TransitionRenderTest, StreamGoesBackwardAndSkipsWhenTheClockJumps)
{
    auto setup = makeSetup("dissolvenza-nero");
    const auto job = makeTransitionJob(setup->project, setup->placement);
    ASSERT_TRUE(job.has_value());
    TransitionPlayback playback;
    playback.setHardwareAllowed(false);
    playback.setBox(kWidth, kHeight);
    // All'indietro dalla fine.
    playback.startStream(*job, 2.5, 0.04, -1);
    for (int k = 0; k <= 10; ++k) {
        const double t = 2.5 - 0.04 * k;
        const auto frame = waitFrame(playback, t, false, 1e-6);
        ASSERT_NE(frame, nullptr) << k;
        EXPECT_NEAR(frame->time, t, 1e-6) << k;
    }
    // In avanti, con il tempo che salta molto più avanti: si arriva al tempo giusto senza fare tutti i fotogrammi.
    playback.startStream(*job, 1.5, 0.04, +1);
    const auto jumped = waitFrame(playback, 2.3, true, 0.05);
    ASSERT_NE(jumped, nullptr);
    EXPECT_NEAR(jumped->time, 2.3, 0.05);
    // Fino alla fine: l'ultimo fotogramma è proprio quello dell'estremo e poi il flusso si ferma.
    const auto last = waitFrame(playback, 2.5, true, 1e-6);
    ASSERT_NE(last, nullptr);
    EXPECT_NEAR(last->time, 2.5, 1e-6);
}
