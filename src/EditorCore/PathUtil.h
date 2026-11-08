#pragma once

// Conversioni fra percorsi e testo UTF-8: in tutto l'editor i percorsi viaggiano come std::string in UTF-8.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace editor::core {

std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// Solo il nome del file (con l'estensione), in UTF-8.
std::string fileNameOf(const std::string& utf8Path);

// Il testo con le lettere ASCII in minuscolo (per confrontare nomi di file e estensioni).
std::string lowercase(std::string text);

// L'estensione del file in minuscolo, con il punto (".mp4"); vuota se non c'è.
std::string extensionOf(const std::string& utf8Path);

// FNV-1a a 64 bit: un codice stabile (lo stesso a ogni avvio) per riconoscere un file dal suo percorso e stato. Per
// continuare un codice su altri dati si passa il risultato precedente come `hash`.
std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t hash = 14695981039346656037ull);

// Chiave per riconoscere lo stesso file: percorso assoluto e normalizzato; su Windows maiuscole e minuscole non
// contano, né "/" o "\".
std::string sameFileKey(const std::string& utf8Path);

// true se i due percorsi indicano lo stesso file (anche se uno dei due non esiste ancora, come il file da creare).
bool samePath(const std::string& a, const std::string& b);

}  // namespace editor::core
