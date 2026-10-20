#include "EditorCore/ProjectFile.h"

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/Json.h"
#include "EditorCore/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace editor::core {

using namespace detail;

namespace {

constexpr const char* kFormatName = "EditorVideo progetto";

std::filesystem::path projectDir(const std::string& projectPath)
{
    std::error_code ec;
    return std::filesystem::absolute(pathFromUtf8(projectPath), ec).parent_path();
}

// Percorso relativo alla cartella del progetto ("" se non si può, es. su un altro disco).
std::string relativeTo(const std::filesystem::path& dir, const std::string& path)
{
    const std::filesystem::path relative = pathFromUtf8(path).lexically_relative(dir);
    if (relative.empty())
        return {};
    return utf8FromPath(relative);
}

bool isFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

// Il file salvato: il percorso assoluto se c'è, altrimenti quello relativo alla cartella del progetto.
std::optional<std::string> locate(const std::filesystem::path& dir, const std::string& absolute, const std::string& relative)
{
    if (!absolute.empty() && isFile(pathFromUtf8(absolute)))
        return absolute;
    if (!relative.empty()) {
        const std::filesystem::path candidate = (dir / pathFromUtf8(relative)).lexically_normal();
        if (isFile(candidate))
            return utf8FromPath(candidate);
    }
    return std::nullopt;
}

JsonValue ticks(double seconds)
{
    return secondsToTicks(seconds);
}

JsonValue optionalTicks(const std::optional<double>& seconds)
{
    return seconds ? JsonValue(secondsToTicks(*seconds)) : JsonValue();
}

double seconds(const JsonValue& value, const char* what)
{
    if (!value.isInteger())
        throw ProjectFileError(std::string("Il progetto è rovinato: manca o non è valido \"") + what + "\".");
    return ticksToSeconds(value.asInteger());
}

std::string text(const JsonValue& value)
{
    return value.isString() ? value.asString() : std::string();
}

}  // namespace

std::int64_t secondsToTicks(double seconds)
{
    return std::llround(seconds * static_cast<double>(kProjectTicksPerSecond));
}

double ticksToSeconds(std::int64_t ticks)
{
    return static_cast<double>(ticks) / static_cast<double>(kProjectTicksPerSecond);
}

std::string projectToJson(const std::string& projectPath, const Project& project, const ProjectView& view)
{
    const std::filesystem::path dir = projectDir(projectPath);

    JsonValue::Array files;
    for (const Clip& clip : project.clips()) {
        JsonValue::Object file{{"id", clip.id}, {"percorso", clip.path}, {"relativo", relativeTo(dir, clip.path)}};
        file["durata"] = clip.analyzed ? ticks(clip.duration) : JsonValue();
        files.emplace_back(std::move(file));
    }

    JsonValue::Array sequence;
    for (const Fragment& f : project.sequence().fragments()) {
        JsonValue::Array muted;
        for (const TimeRange& r : f.muted)
            muted.emplace_back(JsonValue::Array{ticks(r.start), ticks(r.end)});
        sequence.emplace_back(JsonValue::Object{
            {"file", f.clipId}, {"inizio", ticks(f.start)}, {"fine", ticks(f.end)}, {"senzaAudio", std::move(muted)}});
    }

    JsonValue replacement;
    if (const auto& r = project.audioReplacement()) {
        replacement = JsonValue::Object{{"percorso", r->path},
                                        {"relativo", relativeTo(dir, r->path)},
                                        {"scostamento", ticks(r->offset)},
                                        {"durata", ticks(r->duration)}};
    }

    const JsonValue::Object root{
        {"formato", kFormatName},
        {"versione", kProjectFormatVersion},
        {"unitaDiTempo", JsonValue::Object{{"tickAlSecondo", kProjectTicksPerSecond},
                                           {"nota", "i tempi sono numeri interi di tick (1/705600000 di secondo)"}}},
        {"file", std::move(files)},
        {"sequenza", std::move(sequence)},
        {"audioSostitutivo", std::move(replacement)},
        {"sorgente", JsonValue::Object{{"file", view.sourceClipId},
                                       {"in", optionalTicks(view.in)},
                                       {"out", optionalTicks(view.out)},
                                       {"posizione", ticks(view.sourcePosition)}}},
        {"montaggio", JsonValue::Object{{"posizione", ticks(view.programPosition)}}},
    };
    return JsonValue(root).dump();
}

void saveProjectFile(const std::string& projectPath, const Project& project, const ProjectView& view)
{
    const std::string json = projectToJson(projectPath, project, view);
    const std::filesystem::path target = pathFromUtf8(projectPath);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.close();
        if (!out)
            throw ProjectFileError("Impossibile scrivere il progetto: " + projectPath);
    }
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        throw ProjectFileError("Impossibile salvare il progetto in " + projectPath + " (" + ec.message() + ").");
    }
}

LoadedProject loadProjectFile(const std::string& projectPath)
{
    std::ifstream in(pathFromUtf8(projectPath), std::ios::binary);
    if (!in)
        throw ProjectFileError("Impossibile aprire il progetto: " + projectPath);
    std::ostringstream content;
    content << in.rdbuf();
    return projectFromJson(projectPath, content.str());
}

LoadedProject projectFromJson(const std::string& projectPath, const std::string& json)
{
    const std::string name = fileNameOf(projectPath);
    JsonValue root;
    try {
        root = parseJson(json);
    } catch (const JsonError& e) {
        throw ProjectFileError(name + " non è un progetto di EditorVideo valido: il file è rovinato (" + e.what() + ").");
    }
    if (!root.isObject() || text(root["formato"]) != kFormatName)
        throw ProjectFileError(name + " non è un progetto di EditorVideo.");
    if (!root["versione"].isInteger())
        throw ProjectFileError(name + " non è un progetto di EditorVideo valido: manca la versione del formato.");
    const std::int64_t version = root["versione"].asInteger();
    if (version > kProjectFormatVersion || version < 1)
        throw ProjectFileError(name + " è stato salvato con una versione più recente di EditorVideo (formato "
                               + std::to_string(version) + ", questa versione legge fino al "
                               + std::to_string(kProjectFormatVersion) + "): aggiorna il programma per aprirlo.");
    if (root["unitaDiTempo"]["tickAlSecondo"].isInteger()
        && root["unitaDiTempo"]["tickAlSecondo"].asInteger() != kProjectTicksPerSecond)
        throw ProjectFileError(name + " usa un'unità di tempo sconosciuta.");

    const std::filesystem::path dir = projectDir(projectPath);
    LoadedProject loaded;
    try {
        // File: gli id salvati diventano quelli nuovi del progetto, nello stesso ordine.
        std::map<std::int64_t, int> ids;
        for (const JsonValue& file : root["file"].asArray()) {
            const std::string absolute = text(file["percorso"]);
            const std::string relative = text(file["relativo"]);
            if (absolute.empty() && relative.empty())
                throw ProjectFileError(name + " è rovinato: un file non ha il percorso.");
            const auto found = locate(dir, absolute, relative);
            const int id = loaded.project.add(found ? *found : absolute);
            ids[file["id"].asInteger()] = id;
            if (!found)
                loaded.missing.push_back({id, absolute, fileNameOf(absolute.empty() ? relative : absolute)});
            if (file["durata"].isInteger())
                loaded.savedDurations[id] = ticksToSeconds(file["durata"].asInteger());
        }

        std::vector<Fragment> fragments;
        for (const JsonValue& piece : root["sequenza"].asArray()) {
            const auto id = ids.find(piece["file"].asInteger());
            if (id == ids.end())
                throw ProjectFileError(name + " è rovinato: un pezzo della sequenza indica un file che non c'è.");
            Fragment f;
            f.clipId = id->second;
            f.start = seconds(piece["inizio"], "inizio");
            f.end = seconds(piece["fine"], "fine");
            if (!(f.end > f.start))
                throw ProjectFileError(name + " è rovinato: un pezzo della sequenza finisce prima di iniziare.");
            if (piece["senzaAudio"].isArray()) {
                for (const JsonValue& range : piece["senzaAudio"].asArray()) {
                    const auto& pair = range.asArray();
                    if (pair.size() != 2)
                        throw ProjectFileError(name + " è rovinato: un tratto senza audio non è valido.");
                    f.muted.push_back({seconds(pair[0], "senzaAudio"), seconds(pair[1], "senzaAudio")});
                }
            }
            fragments.push_back(std::move(f));
        }
        loaded.project.sequence().restore(std::move(fragments));

        if (const JsonValue& r = root["audioSostitutivo"]; r.isObject()) {
            const std::string absolute = text(r["percorso"]);
            const auto found = locate(dir, absolute, text(r["relativo"]));
            loaded.project.setAudioReplacement(
                AudioReplacement{found ? *found : absolute, seconds(r["scostamento"], "scostamento"),
                                 r["durata"].isInteger() ? ticksToSeconds(r["durata"].asInteger()) : 0.0});
            if (!found)
                loaded.missing.push_back({0, absolute, fileNameOf(absolute)});
        }

        const JsonValue& source = root["sorgente"];
        if (source.isObject()) {
            const auto id = source["file"].isInteger() ? ids.find(source["file"].asInteger()) : ids.end();
            loaded.view.sourceClipId = id != ids.end() ? id->second : 0;
            if (source["in"].isInteger())
                loaded.view.in = ticksToSeconds(source["in"].asInteger());
            if (source["out"].isInteger())
                loaded.view.out = ticksToSeconds(source["out"].asInteger());
            if (source["posizione"].isInteger())
                loaded.view.sourcePosition = ticksToSeconds(source["posizione"].asInteger());
        }
        if (root["montaggio"]["posizione"].isInteger())
            loaded.view.programPosition = ticksToSeconds(root["montaggio"]["posizione"].asInteger());
    } catch (const JsonError& e) {
        throw ProjectFileError(name + " non è un progetto di EditorVideo valido (" + e.what() + ").");
    }
    return loaded;
}

void relocateMissing(LoadedProject& loaded, const MissingFile& file, const std::string& newPath)
{
    if (file.clipId == 0) {
        if (auto replacement = loaded.project.audioReplacement()) {
            replacement->path = newPath;
            loaded.project.setAudioReplacement(replacement);
        }
    } else {
        loaded.project.setClipPath(file.clipId, newPath);
    }
    std::erase_if(loaded.missing, [&](const MissingFile& m) { return m.clipId == file.clipId; });
}

int dropMissing(LoadedProject& loaded)
{
    int removed = 0;
    for (const MissingFile& file : loaded.missing) {
        if (file.clipId == 0) {
            loaded.project.setAudioReplacement(std::nullopt);
            continue;
        }
        const auto& fragments = loaded.project.sequence().fragments();
        removed += static_cast<int>(std::count_if(fragments.begin(), fragments.end(),
                                                  [&](const Fragment& f) { return f.clipId == file.clipId; }));
        loaded.project.remove(file.clipId);
        loaded.savedDurations.erase(file.clipId);
        if (loaded.view.sourceClipId == file.clipId)
            loaded.view.sourceClipId = 0;
    }
    loaded.missing.clear();
    return removed;
}

}  // namespace editor::core
