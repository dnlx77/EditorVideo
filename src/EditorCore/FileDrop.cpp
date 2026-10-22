#include "EditorCore/FileDrop.h"

#include "EditorCore/FfmpegUtil.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/ProjectFile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace editor::core {

using namespace detail;

namespace {

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string extensionOf(const std::string& utf8Path)
{
    return lowercase(utf8FromPath(pathFromUtf8(utf8Path).extension()));
}

// Chiave per riconoscere lo stesso file: su Windows maiuscole e minuscole non contano, né "/" o "\".
std::string sameFileKey(const std::string& utf8Path)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(pathFromUtf8(utf8Path), ec).lexically_normal();
    return lowercase(utf8FromPath(absolute.make_preferred()));
}

}  // namespace

const std::vector<std::string>& mediaExtensions()
{
    static const std::vector<std::string> extensions = {
        ".mp4", ".mkv", ".mov", ".avi", ".m4v", ".webm", ".ts", ".mts", ".m2ts", ".mpg", ".mpeg", ".wmv", ".flv",
        ".3gp", ".mp3", ".m4a", ".aac", ".wav", ".flac", ".ogg", ".opus", ".wma", ".ac3", ".mka"};
    return extensions;
}

DropPlan planDrop(const std::vector<std::string>& paths, const Project& project, const MediaInspector& inspect)
{
    DropPlan plan;
    std::vector<std::string> files;
    for (const std::string& path : paths) {
        std::error_code ec;
        const std::filesystem::path p = pathFromUtf8(path);
        if (extensionOf(path) == kProjectExtension && std::filesystem::is_regular_file(p, ec)) {
            plan.project = utf8FromPath(std::filesystem::absolute(p, ec));
            plan.media.clear();
            plan.unreadable.clear();
            return plan;
        }
        if (std::filesystem::is_directory(p, ec)) {
            // Una cartella: i video e gli audio che contiene, in ordine di nome.
            std::vector<std::string> inside;
            for (const auto& entry : std::filesystem::directory_iterator(p, ec)) {
                const std::string file = utf8FromPath(entry.path());
                const auto& known = mediaExtensions();
                if (entry.is_regular_file(ec) && std::find(known.begin(), known.end(), extensionOf(file)) != known.end())
                    inside.push_back(file);
            }
            std::sort(inside.begin(), inside.end(),
                      [](const std::string& a, const std::string& b) { return lowercase(a) < lowercase(b); });
            files.insert(files.end(), inside.begin(), inside.end());
            continue;
        }
        files.push_back(path);
    }

    std::vector<std::string> seen;
    for (const std::string& file : files) {
        const std::string key = sameFileKey(file);
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;  // trascinato due volte (es. il file e la sua cartella)
        seen.push_back(key);

        const auto existing = std::find_if(project.clips().begin(), project.clips().end(),
                                           [&](const Clip& clip) { return sameFileKey(clip.path) == key; });
        if (existing != project.clips().end()) {
            // Già nel progetto: non si aggiunge, ma può andare nella sequenza.
            plan.media.push_back({existing->path, existing->analyzed ? existing->duration : inspect(existing->path).value_or(0.0),
                                  existing->id});
            continue;
        }
        if (const auto duration = inspect(file))
            plan.media.push_back({utf8FromPath(std::filesystem::absolute(pathFromUtf8(file))), *duration, 0});
        else
            plan.unreadable.push_back(fileNameOf(file));
    }
    return plan;
}

std::optional<double> inspectMedia(const std::string& utf8Path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pathFromUtf8(utf8Path), ec))
        return std::nullopt;
    try {
        const FormatContextPtr ctx = openInput(utf8Path);
        if (findMainStream(ctx.get(), AVMEDIA_TYPE_VIDEO) < 0 && findMainStream(ctx.get(), AVMEDIA_TYPE_AUDIO) < 0)
            return std::nullopt;
        return ctx->duration != AV_NOPTS_VALUE ? std::max(0.0, ctx->duration / double(AV_TIME_BASE)) : 0.0;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<int> applyDrop(Project& project, const std::vector<DroppedMedia>& media, int sequenceIndex,
                           std::vector<int>* fragments)
{
    std::vector<int> ids;
    int at = sequenceIndex;
    for (const DroppedMedia& m : media) {
        const int id = m.existingClipId != 0 ? m.existingClipId : project.add(m.path);
        ids.push_back(id);
        if (sequenceIndex < 0)
            continue;
        // Il file intero.
        const Clip* clip = project.find(id);
        const double duration = clip != nullptr && clip->analyzed ? clip->duration : m.duration;
        const int fragment = project.sequence().insert(at, id, 0.0, duration);
        if (fragment != 0) {
            if (fragments != nullptr)
                fragments->push_back(fragment);
            at = project.sequence().indexOf(fragment) + 1;
        }
    }
    return ids;
}

}  // namespace editor::core
