#include "EditorCore/PathUtil.h"

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

}  // namespace editor::core
