#include "EditorCore/Compatibility.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/SegmentExport.h"
#include "MediaChecks.h"
#include "SampleMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::decodeVideoFrames;
using editor::tests::lumaAt;
using editor::tests::writeEncodedClip;

namespace {

// File di prova H.264 (con la scheda video): 4 s a 30 fotogrammi al secondo, un fotogramma chiave al secondo
// (0, 1, 2, 3 s), luminanza uniforme 40 + numero del fotogramma, audio AAC.
constexpr int kFps = 30;
constexpr int kFrames = 120;

int lumaOf(int index)
{
    return 40 + index;
}

bool writeClip(const fs::path& path, int bFrames)
{
    return writeEncodedClip(path, {.fps = kFps, .frames = kFrames, .gop = kFps, .bFrames = bFrames, .luma = lumaOf(0), .lumaStep = 1});
}

struct Decoded
{
    std::vector<int> luma;  // al centro, per ogni fotogramma in ordine di visualizzazione
    int errors = 0;
};

Decoded decodeAll(const fs::path& path)
{
    Decoded d;
    d.errors = decodeVideoFrames(path, [&](const AVFrame& f) { d.luma.push_back(lumaAt(f, f.width / 2, f.height / 2)); }, true);
    return d;
}

// I fotogrammi da `first` (compreso) a `last` (escluso) del file di prova.
std::vector<int> frames(int first, int last)
{
    std::vector<int> out;
    for (int i = first; i < last; ++i)
        out.push_back(lumaOf(i));
    return out;
}

std::vector<int> concat(std::initializer_list<std::vector<int>> parts)
{
    std::vector<int> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Quanti fotogrammi hanno una luminanza diversa da quella attesa (o mancano).
int wrongFrames(const std::vector<int>& got, const std::vector<int>& expected)
{
    int wrong = static_cast<int>(std::max(got.size(), expected.size()) - std::min(got.size(), expected.size()));
    for (std::size_t i = 0; i < std::min(got.size(), expected.size()); ++i)
        wrong += std::abs(got[i] - expected[i]) > 3 ? 1 : 0;
    return wrong;
}

std::size_t rangeCount(const std::vector<ClipRanges>& plan)
{
    std::size_t count = 0;
    for (const ClipRanges& clip : plan)
        count += clip.ranges.size();
    return count;
}

class PreciseCut : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / "EditorVideoTaglioPreciso";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!preferredVideoEncoder(VideoCodec::H264))
            return;
        ok_ = writeClip(dir_ / "senza_b.mp4", 0);
        // Con i B-frame (se l'encoder li fa davvero): per la fine del tratto.
        bFrames_ = ok_ && writeClip(dir_ / "con_b.mp4", 2)
                && readMediaSignature(utf8FromPath(dir_ / "con_b.mp4")).streams[0].bFrames;
    }

    void SetUp() override
    {
        if (!ok_)
            GTEST_SKIP() << "Nessun encoder H.264 su questo PC: le prove del taglio preciso si saltano.";
    }

    static std::string file(const char* name) { return utf8FromPath(dir_ / name); }

    // Esporta e controlla: fotogrammi attesi, nessun errore, audio lungo quanto il video e senza sovrapposizioni.
    static void check(const std::vector<ClipRanges>& plan, const std::vector<int>& expected, int preciseCuts,
                      const char* name)
    {
        for (const char* extension : {".mp4", ".mkv"}) {
            SCOPED_TRACE(std::string(name) + extension);
            const fs::path out = dir_ / (std::string(name) + extension);
            const ExportResult result = exportClips(plan, utf8FromPath(out), AudioEdit{});
            EXPECT_EQ(result.preciseCuts, preciseCuts);
            const Decoded decoded = decodeAll(out);
            EXPECT_EQ(decoded.errors, 0);
            EXPECT_EQ(wrongFrames(decoded.luma, expected), 0)
                << "primo fotogramma " << (decoded.luma.empty() ? -1 : decoded.luma.front()) << ", atteso "
                << (expected.empty() ? -1 : expected.front()) << "; fotogrammi " << decoded.luma.size() << " su "
                << expected.size();
            const double seconds = static_cast<double>(expected.size()) / kFps;
            EXPECT_NEAR(probeFile(utf8FromPath(out)).durationSeconds, seconds, 0.05);
            const editor::tests::AudioTimeline audio = editor::tests::audioTimeline(utf8FromPath(out));
            // L'audio compresso si taglia a blocchi (AAC: ~21 ms): al più un blocco perso a ogni bordo esterno dei tratti (i
            // bordi del taglio preciso, dentro un tratto, non perdono niente).
            EXPECT_NEAR(audio.totalSeconds, seconds, 0.022 * 2 * static_cast<double>(rangeCount(plan)) + 0.005);
            EXPECT_FALSE(audio.overlaps);
        }
    }

    static inline fs::path dir_;
    static inline bool ok_ = false;
    static inline bool bFrames_ = false;
};

TEST_F(PreciseCut, InBetweenKeyframesStartsAtTheChosenFrame)
{
    // IN a 1,5 s (fotogramma 45), fra i fotogrammi chiave 1 e 2: si ricodifica 1,5 .. 2, poi si copia 2 .. 3.
    const std::vector<ClipRanges> plan{{file("senza_b.mp4"), {{1.5, 3.0}}}};
    const ExportPreview preview = previewExport(plan, AudioEdit{}, nullptr);
    EXPECT_EQ(preview.preciseCuts, 1);
    EXPECT_NEAR(preview.preciseSeconds, 0.5, 1e-9);
    EXPECT_TRUE(preview.impreciseCuts.empty());
    check(plan, frames(45, 90), 1, "in_preciso");
}

TEST_F(PreciseCut, InAndOutInTheSameGopAreReencodedWhole)
{
    check({{file("senza_b.mp4"), {{1.2, 1.8}}}}, frames(36, 54), 1, "stesso_gop");
}

TEST_F(PreciseCut, OutWithBFramesReencodesTheLastPart)
{
    if (!bFrames_)
        GTEST_SKIP() << "L'encoder non ha fatto un file con i B-frame.";
    // OUT a 2,5 s: si copia 1 .. 2 e si ricodifica 2 .. 2,5 (i B-frame dopo il fotogramma chiave guardano avanti).
    check({{file("con_b.mp4"), {{1.0, 2.5}}}}, frames(30, 75), 1, "out_con_b");
}

TEST_F(PreciseCut, JoinsKeepFramesAndSound)
{
    // Due tagli precisi in un montaggio, anche dello stesso file all'indietro.
    const std::vector<ClipRanges> plan{{file("senza_b.mp4"), {{1.5, 3.0}, {0.2, 1.3}}}};
    check(plan, concat({frames(45, 90), frames(6, 39)}), 2, "montaggio");
}

}  // namespace
