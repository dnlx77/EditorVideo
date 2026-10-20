#pragma once

// Conversioni fra percorsi e testo UTF-8: in tutto l'editor i percorsi viaggiano come std::string in UTF-8.

#include <filesystem>
#include <string>

namespace editor::core {

std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// Solo il nome del file (con l'estensione), in UTF-8.
std::string fileNameOf(const std::string& utf8Path);

}  // namespace editor::core
