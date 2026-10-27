#include "EditorCore/KeyframeCache.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "SampleMedia.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace editor::core;
using editor::tests::SampleDirTest;
using editor::tests::writeSampleFile;

namespace {

class KeyframeCacheTest : public SampleDirTest<KeyframeCacheTest>
{
public:
    static constexpr const char* kDirName = "EditorVideoKeyframeCacheTests";

    static void prepareSamples()
    {
        writeSampleFile(dir_ / "clip.mkv", {.video = true, .audio = true, .frames = 100, .gopSize = 25});
    }

    // Cartella di cache vuota per ogni prova.
    std::string freshCache(const char* name) const
    {
        const fs::path folder = dir_ / name;
        fs::remove_all(folder);
        return utf8FromPath(folder);
    }

    static std::vector<fs::path> cacheFiles(const std::string& folder)
    {
        std::vector<fs::path> files;
        if (fs::exists(pathFromUtf8(folder)))
            for (const auto& entry : fs::directory_iterator(pathFromUtf8(folder)))
                files.push_back(entry.path());
        return files;
    }
};

TEST_F(KeyframeCacheTest, ProbeSavesAndReusesKeyframes)
{
    const std::string cache = freshCache("cache1");
    const MediaInfo first = probeFile(path("clip.mkv"), nullptr, cache);
    ASSERT_EQ(first.keyframeTimes.size(), 4u);

    const auto files = cacheFiles(cache);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].extension(), ".kfcache");

    int stream = -1;
    std::vector<double> times;
    ASSERT_TRUE(loadKeyframeCache(cache, path("clip.mkv"), stream, times));
    EXPECT_EQ(stream, first.keyframeStreamIndex);
    EXPECT_EQ(times, first.keyframeTimes);

    // Con la cache si ottiene lo stesso risultato.
    const MediaInfo second = probeFile(path("clip.mkv"), nullptr, cache);
    EXPECT_EQ(second.keyframeTimes, first.keyframeTimes);
    EXPECT_EQ(second.keyframeStreamIndex, first.keyframeStreamIndex);
}

TEST_F(KeyframeCacheTest, CacheContentIsUsedWhenPresent)
{
    // Prova che la lettura viene davvero dalla cache: con tempi finti salvati a mano il risultato è quello finto.
    const std::string cache = freshCache("cache2");
    const MediaInfo real = probeFile(path("clip.mkv"));
    ASSERT_TRUE(saveKeyframeCache(cache, path("clip.mkv"), real.keyframeStreamIndex, {0.0, 1.5}));
    const MediaInfo cached = probeFile(path("clip.mkv"), nullptr, cache);
    EXPECT_EQ(cached.keyframeTimes, (std::vector<double>{0.0, 1.5}));
}

TEST_F(KeyframeCacheTest, ChangedFileDiscardsCache)
{
    const fs::path copy = dir_ / "cambia.mkv";
    fs::copy_file(dir_ / "clip.mkv", copy, fs::copy_options::overwrite_existing);
    const std::string cache = freshCache("cache3");
    ASSERT_EQ(probeFile(utf8FromPath(copy), nullptr, cache).keyframeTimes.size(), 4u);

    // Cambia la data di modifica: la cache non corrisponde più.
    fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::hours(1));
    int stream = -1;
    std::vector<double> times;
    EXPECT_FALSE(loadKeyframeCache(cache, utf8FromPath(copy), stream, times));

    // Cambia il contenuto (dimensione diversa).
    writeSampleFile(copy, {.video = true, .audio = true, .frames = 50, .gopSize = 25});
    EXPECT_EQ(probeFile(utf8FromPath(copy), nullptr, cache).keyframeTimes.size(), 2u);
}

TEST_F(KeyframeCacheTest, CorruptOrTruncatedCacheFallsBackToScan)
{
    const std::string cache = freshCache("cache4");
    const MediaInfo real = probeFile(path("clip.mkv"), nullptr, cache);
    const auto files = cacheFiles(cache);
    ASSERT_EQ(files.size(), 1u);

    // Un byte cambiato nei dati.
    {
        std::fstream f(files[0], std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-12, std::ios::end);
        f.put('\x7f');
    }
    int stream = -1;
    std::vector<double> times;
    EXPECT_FALSE(loadKeyframeCache(cache, path("clip.mkv"), stream, times));
    EXPECT_EQ(probeFile(path("clip.mkv"), nullptr, cache).keyframeTimes, real.keyframeTimes);

    // Ora la cache è stata rifatta: la tronchiamo.
    ASSERT_TRUE(loadKeyframeCache(cache, path("clip.mkv"), stream, times));
    fs::resize_file(files[0], 10);
    EXPECT_FALSE(loadKeyframeCache(cache, path("clip.mkv"), stream, times));
    EXPECT_EQ(probeFile(path("clip.mkv"), nullptr, cache).keyframeTimes, real.keyframeTimes);

    // File vuoto, o non è una cache.
    fs::resize_file(files[0], 0);
    EXPECT_FALSE(loadKeyframeCache(cache, path("clip.mkv"), stream, times));
}

TEST_F(KeyframeCacheTest, NoCacheFolderOrMissingFile)
{
    int stream = -1;
    std::vector<double> times;
    EXPECT_FALSE(loadKeyframeCache("", path("clip.mkv"), stream, times));
    EXPECT_FALSE(saveKeyframeCache("", path("clip.mkv"), 0, {1.0}));
    EXPECT_TRUE(keyframeCacheFileName(path("non_esiste.mkv")).empty());
    EXPECT_EQ(probeFile(path("clip.mkv")).keyframeTimes.size(), 4u);
}

}  // namespace
