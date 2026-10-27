#include "EditorCore/KeyframeCache.h"

#include "EditorCore/PathUtil.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace editor::core {

namespace {

namespace fs = std::filesystem;

constexpr char kMagic[4] = {'E', 'V', 'K', 'F'};
constexpr std::uint32_t kVersion = 1;

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

struct FileState
{
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

bool stateOf(const fs::path& file, FileState& state)
{
    std::error_code ec;
    state.size = fs::file_size(file, ec);
    if (ec)
        return false;
    const auto modified = fs::last_write_time(file, ec);
    if (ec)
        return false;
    state.modified = static_cast<std::int64_t>(modified.time_since_epoch().count());
    return true;
}

#pragma pack(push, 1)
struct Header
{
    char magic[4];
    std::uint32_t version;
    std::uint64_t mediaSize;
    std::int64_t mediaModified;
    std::int32_t streamIndex;
    std::uint64_t count;
};
#pragma pack(pop)

std::uint64_t checksum(const Header& header, const std::vector<double>& times)
{
    std::uint64_t hash = fnv1a(&header, sizeof header);
    return fnv1a(times.data(), times.size() * sizeof(double), hash);
}

}  // namespace

std::string keyframeCacheFileName(const std::string& mediaUtf8Path)
{
    const fs::path media = pathFromUtf8(mediaUtf8Path);
    FileState state;
    if (!stateOf(media, state))
        return {};
    const std::string key =
        sameFileKey(mediaUtf8Path) + "|" + std::to_string(state.size) + "|" + std::to_string(state.modified);
    char code[17];
    std::snprintf(code, sizeof code, "%016llx", static_cast<unsigned long long>(fnv1a(key.data(), key.size())));
    return utf8FromPath(media.stem()) + "_" + code + ".kfcache";
}

bool loadKeyframeCache(const std::string& cacheFolder, const std::string& mediaUtf8Path, int& streamIndex,
                       std::vector<double>& times)
{
    try {
        const std::string name = keyframeCacheFileName(mediaUtf8Path);
        FileState state;
        if (cacheFolder.empty() || name.empty() || !stateOf(pathFromUtf8(mediaUtf8Path), state))
            return false;

        std::ifstream in(pathFromUtf8(cacheFolder) / pathFromUtf8(name), std::ios::binary);
        if (!in)
            return false;
        Header header;
        if (!in.read(reinterpret_cast<char*>(&header), sizeof header))
            return false;
        if (std::memcmp(header.magic, kMagic, sizeof kMagic) != 0 || header.version != kVersion ||
            header.mediaSize != state.size || header.mediaModified != state.modified || header.streamIndex < 0)
            return false;

        // Il numero dichiarato deve stare nel file: così un'intestazione rovinata non fa allocare memoria a vuoto.
        in.seekg(0, std::ios::end);
        const std::uint64_t fileSize = static_cast<std::uint64_t>(in.tellg());
        const std::uint64_t expected = sizeof header + header.count * sizeof(double) + sizeof(std::uint64_t);
        if (header.count > fileSize / sizeof(double) || fileSize != expected)
            return false;
        in.seekg(sizeof header);

        std::vector<double> loaded(static_cast<std::size_t>(header.count));
        std::uint64_t stored = 0;
        if (!loaded.empty() && !in.read(reinterpret_cast<char*>(loaded.data()), loaded.size() * sizeof(double)))
            return false;
        if (!in.read(reinterpret_cast<char*>(&stored), sizeof stored) || stored != checksum(header, loaded))
            return false;

        streamIndex = header.streamIndex;
        times = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

bool saveKeyframeCache(const std::string& cacheFolder, const std::string& mediaUtf8Path, int streamIndex,
                       const std::vector<double>& times)
{
    try {
        const std::string name = keyframeCacheFileName(mediaUtf8Path);
        FileState state;
        if (cacheFolder.empty() || name.empty() || streamIndex < 0 || !stateOf(pathFromUtf8(mediaUtf8Path), state))
            return false;

        const fs::path folder = pathFromUtf8(cacheFolder);
        std::error_code ec;
        fs::create_directories(folder, ec);
        const fs::path target = folder / pathFromUtf8(name);
        fs::path part = target;
        part += ".part";

        Header header{};
        std::memcpy(header.magic, kMagic, sizeof kMagic);
        header.version = kVersion;
        header.mediaSize = state.size;
        header.mediaModified = state.modified;
        header.streamIndex = streamIndex;
        header.count = times.size();
        const std::uint64_t sum = checksum(header, times);
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(reinterpret_cast<const char*>(&header), sizeof header);
            out.write(reinterpret_cast<const char*>(times.data()), static_cast<std::streamsize>(times.size() * sizeof(double)));
            out.write(reinterpret_cast<const char*>(&sum), sizeof sum);
            out.flush();
            if (!out) {
                out.close();
                fs::remove(part, ec);
                return false;
            }
        }
        fs::rename(part, target, ec);
        if (ec) {
            fs::remove(part, ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace editor::core
