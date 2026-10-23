#pragma once

// Conversioni fra percorsi e testo UTF-8: in tutto l'editor i percorsi viaggiano come std::string in UTF-8.

#include <filesystem>
#include <string>

namespace editor::core {

std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// Solo il nome del file (con l'estensione), in UTF-8.
std::string fileNameOf(const std::string& utf8Path);

// Il testo con le lettere ASCII in minuscolo (per confrontare nomi di file e estensioni).
std::string lowercase(std::string text);

// Chiave per riconoscere lo stesso file: percorso assoluto e normalizzato; su Windows maiuscole e minuscole non
// contano, né "/" o "\".
std::string sameFileKey(const std::string& utf8Path);

// true se i due percorsi indicano lo stesso file (anche se uno dei due non esiste ancora, come il file da creare).
bool samePath(const std::string& a, const std::string& b);

}  // namespace editor::core
