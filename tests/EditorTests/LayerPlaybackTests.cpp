#include "EditorCore/LayerPlayback.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace editor::core;
using editor::tests::SampleDirTest;
using editor::tests::writeSampleFile;

namespace {

constexpr double kFrame = 1.0 / 25.0;

class LayerPlaybackTest : public SampleDirTest<LayerPlaybackTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoLayerPlaybackTests";

    static void prepareSamples()
    {
        // 4 s a 25 fps, un fotogramma chiave ogni secondo.
        writeSampleFile(dir_ / "livello.mkv", {.video = true, .audio = false, .frames = 100, .gopSize = 25});
    }
};

}  // namespace

TEST_F(LayerPlaybackTest, PausedFramesComeFromTheRequestedTime)
{
    LayerPlayback playback((dir_ / "livello.mkv").string(), 64, 48, false);
    const auto a = playback.waitFrame(1.0);
    ASSERT_NE(a, nullptr);
    EXPECT_NEAR(a->time, 1.0, kFrame);
    const auto b = playback.waitFrame(2.5);
    ASSERT_NE(b, nullptr);
    EXPECT_NEAR(b->time, 2.5, kFrame);
    // Indietro.
    const auto c = playback.waitFrame(0.4);
    ASSERT_NE(c, nullptr);
    EXPECT_NEAR(c->time, 0.4, kFrame);
    EXPECT_FALSE(playback.failed());
}

TEST_F(LayerPlaybackTest, PlayingStreamsFramesInOrder)
{
    LayerPlayback playback((dir_ / "livello.mkv").string(), 64, 48, false);
    ASSERT_NE(playback.waitFrame(0.0), nullptr);
    double last = -1.0;
    int shown = 0;
    // Il tempo scorre da 0 a 1,5 s a passi di un fotogramma: i fotogrammi arrivano in ordine e senza saltare (la decodifica
    // di un file piccolo sta al passo).
    for (double t = 0.0; t < 1.5; t += kFrame) {
        playback.request(t, true);
        std::shared_ptr<VideoFrame> f;
        for (int wait = 0; wait < 200 && (f = playback.frame(t)) == nullptr; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ASSERT_NE(f, nullptr) << "t=" << t;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // (il doppio del tempo reale)
        EXPECT_LE(f->time, t + 1e-6);
        EXPECT_GE(f->time + 6 * kFrame, t);
        EXPECT_GE(f->time, last - 1e-9);
        last = f->time;
        ++shown;
    }
    EXPECT_GT(shown, 30);
    // Un salto avanti in riproduzione: si arriva lì.
    playback.request(3.0, true);
    std::shared_ptr<VideoFrame> f;
    for (int wait = 0; wait < 400 && (f = playback.frame(3.0)) == nullptr; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_NE(f, nullptr);
    EXPECT_NEAR(f->time, 3.0, 3 * kFrame);
}

TEST_F(LayerPlaybackTest, MissingFileFailsWithoutBlocking)
{
    LayerPlayback playback((dir_ / "non_esiste.mkv").string(), 64, 48, false);
    EXPECT_EQ(playback.waitFrame(0.0, 3000), nullptr);
    EXPECT_TRUE(playback.failed());
}
