#pragma once

#include "EditorCore/Project.h"

namespace editor::tests {

// Un progetto di prova senza file veri: tre pezzi video di 2 s (dallo stesso "video.mp4") e una clip audio su una
// traccia A2. Usato dalle prove dei gruppi e dei movimenti di gruppo.
struct Montage
{
    // Adjacent: pezzi attaccati (0 .. 2, 2 .. 4, 4 .. 6) e clip audio 1 .. 4 (lunga 3 s).
    // Spaced: 1 s di spazio fra i pezzi (0 .. 2, 3 .. 5, 6 .. 8) e clip audio 3 .. 5 (lunga 2 s).
    enum class Layout { Adjacent, Spaced };

    explicit Montage(Layout layout = Layout::Adjacent)
    {
        const int file = project.add("video.mp4");
        project.setAnalysis(file, {0.0}, 30.0);
        a = project.sequence().insert(-1, file, 0.0, 2.0);
        b = project.sequence().insert(-1, file, 4.0, 6.0);
        c = project.sequence().insert(-1, file, 8.0, 10.0);
        if (layout == Layout::Spaced) {
            project.sequence().moveTo(b, 3.0);
            project.sequence().moveTo(c, 6.0);
        }
        core::AudioClip clip;
        clip.path = "musica.mp3";
        clip.end = layout == Layout::Spaced ? 2.0 : 3.0;
        clip.position = layout == Layout::Spaced ? 3.0 : 1.0;
        clip.sourceDuration = 100.0;
        track = project.audioTracks().addTrack();
        music = project.audioTracks().add(track, clip);
    }

    double start(int id) const { return project.sequence().startOf(project.sequence().indexOf(id)); }
    double clipAt() const { return project.audioTracks().find(music)->position; }

    core::Project project;
    int a = 0;
    int b = 0;
    int c = 0;
    int music = 0;  // la clip audio
    int track = 0;  // la sua traccia
};

}  // namespace editor::tests
