#include "EditorCore/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace editor::core {

std::filesystem::path pathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8FromPath(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string fileNameOf(const std::string& utf8Path)
{
    return utf8FromPath(pathFromUtf8(utf8Path).filename());
}

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string extensionOf(const std::string& utf8Path)
{
    return lowercase(utf8FromPath(pathFromUtf8(utf8Path).extension()));
}

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t hash)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string sameFileKey(const std::string& utf8Path)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::weakly_canonical(pathFromUtf8(utf8Path), ec);
    if (ec)
        path = std::filesystem::absolute(pathFromUtf8(utf8Path), ec).lexically_normal();
    return lowercase(utf8FromPath(path.make_preferred()));
}

bool samePath(const std::string& a, const std::string& b)
{
    std::error_code ec;
    const std::filesystem::path pa = pathFromUtf8(a);
    const std::filesystem::path pb = pathFromUtf8(b);
    if (std::filesystem::exists(pa, ec) && std::filesystem::exists(pb, ec) && std::filesystem::equivalent(pa, pb, ec))
        return true;
    return sameFileKey(a) == sameFileKey(b);
}

}  // namespace editor::core
