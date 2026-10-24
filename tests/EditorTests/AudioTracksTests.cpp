#include "EditorCore/AudioTracks.h"
#include "EditorCore/EditHistory.h"
#include "EditorCore/Project.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

AudioClip clipOf(const char* path, double start, double end, double position, double sourceDuration = 60.0)
{
    AudioClip clip;
    clip.path = path;
    clip.start = start;
    clip.end = end;
    clip.position = position;
    clip.sourceDuration = sourceDuration;
    return clip;
}

TEST(AudioTracks, TracksHaveNamesLevelsMuteAndSolo)
{
    AudioTracks audio;
    EXPECT_TRUE(audio.isDefault());
    const int a2 = audio.addTrack();
    const int a3 = audio.addTrack();
    EXPECT_FALSE(audio.isDefault());
    EXPECT_EQ(audio.trackName(AudioTracks::kOriginalTrack), "A1");
    EXPECT_EQ(audio.trackName(a2), "A2");
    EXPECT_EQ(audio.trackName(a3), "A3");

    // Livello in dB, limitato.
    EXPECT_TRUE(audio.setLevel(a2, -6.0));
    EXPECT_NEAR(audio.gain(a2), 0.501, 0.001);
    EXPECT_TRUE(audio.setLevel(a3, 40.0));
    EXPECT_EQ(audio.mix(a3).levelDb, AudioTracks::kMaxLevelDb);
    EXPECT_FALSE(audio.setLevel(a3, 40.0));  // nessun cambiamento
    EXPECT_DOUBLE_EQ(audio.gain(AudioTracks::kOriginalTrack), 1.0);

    // Muto e solo: con una traccia in solo si sentono solo quelle in solo (non mute).
    EXPECT_TRUE(audio.setMuted(AudioTracks::kOriginalTrack, true));
    EXPECT_FALSE(audio.audible(AudioTracks::kOriginalTrack));
    EXPECT_TRUE(audio.audible(a2));
    EXPECT_TRUE(audio.setSolo(a3, true));
    EXPECT_FALSE(audio.audible(a2));
    EXPECT_TRUE(audio.audible(a3));
    EXPECT_EQ(audio.gain(a2), 0.0);
    EXPECT_TRUE(audio.setSolo(a2, true));
    EXPECT_TRUE(audio.audible(a2));
    EXPECT_TRUE(audio.setMuted(a2, true));  // muto vince su solo
    EXPECT_FALSE(audio.audible(a2));
    EXPECT_FALSE(audio.audible(99));  // traccia che non c'è

    // Togliendo A2, A3 diventa A2.
    EXPECT_TRUE(audio.removeTrack(a2));
    EXPECT_EQ(audio.trackName(a3), "A2");
    EXPECT_EQ(audio.trackName(a2), "");
    EXPECT_FALSE(audio.setLevel(a2, 0.0));
}

TEST(AudioTracks, ClipsDoNotOverlapOnTheSameTrack)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    const int a3 = audio.addTrack();
    const int first = audio.add(a2, clipOf("voce.wav", 0.0, 4.0, 1.0));
    ASSERT_NE(first, 0);
    EXPECT_EQ(audio.add(a2, clipOf("musica.mp3", 0.0, 2.0, 4.0)), 0);  // si sovrapporrebbe
    const int second = audio.add(a2, clipOf("musica.mp3", 0.0, 2.0, 5.0));  // attaccata alla prima
    ASSERT_NE(second, 0);
    EXPECT_NE(audio.add(a3, clipOf("musica.mp3", 0.0, 2.0, 4.0)), 0);  // su un'altra traccia sì
    EXPECT_EQ(audio.add(a2, clipOf("vuota.wav", 1.0, 1.0, 20.0)), 0);
    EXPECT_EQ(audio.add(99, clipOf("voce.wav", 0.0, 1.0, 20.0)), 0);
    EXPECT_EQ(audio.trackOf(second), a2);
    EXPECT_DOUBLE_EQ(audio.endTime(), 7.0);

    // Il punto libero più vicino: contro la clip che blocca.
    EXPECT_DOUBLE_EQ(audio.nearestFreePosition(second, a2, 9.0), 9.0);
    EXPECT_DOUBLE_EQ(audio.nearestFreePosition(second, a2, 4.5), 5.0);  // dopo la prima
    EXPECT_DOUBLE_EQ(audio.nearestFreePosition(second, a2, 1.5), 5.0);  // prima della prima non ci sta
    EXPECT_DOUBLE_EQ(audio.nearestFreePosition(second, a3, 3.5), 2.0);  // su A3: prima di quella che c'è
    EXPECT_DOUBLE_EQ(audio.nearestFreePosition(second, a2, -3.0), 5.0);
    EXPECT_DOUBLE_EQ(audio.freePosition(a2, 1.0, 4.5), 7.0);   // una clip nuova di 1 s: dopo le due
    EXPECT_DOUBLE_EQ(audio.freePosition(a2, 1.0, 0.2), 0.0);   // prima della prima, a 0
    EXPECT_DOUBLE_EQ(audio.freePosition(99, 1.0, 0.0), -1.0);

    // Spostare: non sopra un'altra; anche su un'altra traccia.
    EXPECT_FALSE(audio.moveTo(second, a2, 3.0));
    EXPECT_DOUBLE_EQ(audio.find(second)->position, 5.0);
    EXPECT_TRUE(audio.moveTo(second, a2, 8.0));
    EXPECT_FALSE(audio.moveTo(second, a3, 5.0));
    EXPECT_TRUE(audio.moveTo(second, a3, 0.0));
    EXPECT_EQ(audio.trackOf(second), a3);
    EXPECT_EQ(audio.track(a3)->clips.front().id, second);  // in ordine di posizione
    EXPECT_EQ(audio.track(a2)->clips.size(), 1u);

    EXPECT_TRUE(audio.remove(first));
    EXPECT_EQ(audio.find(first), nullptr);
    EXPECT_FALSE(audio.remove(first));
}

TEST(AudioTracks, SplitLeavesTwoClipsThatPlayTheSame)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    const int id = audio.add(a2, clipOf("voce.wav", 2.0, 8.0, 10.0));
    EXPECT_EQ(audio.split(a2, 10.0), 0);  // sul bordo
    EXPECT_EQ(audio.split(a2, 3.0), 0);   // fuori
    const int second = audio.split(a2, 12.5);
    ASSERT_NE(second, 0);
    const AudioClip& a = *audio.find(id);
    const AudioClip& b = *audio.find(second);
    EXPECT_DOUBLE_EQ(a.start, 2.0);
    EXPECT_DOUBLE_EQ(a.end, 4.5);
    EXPECT_DOUBLE_EQ(b.start, 4.5);
    EXPECT_DOUBLE_EQ(b.end, 8.0);
    EXPECT_DOUBLE_EQ(b.position, 12.5);
    EXPECT_DOUBLE_EQ(b.endPosition(), 16.0);
    EXPECT_EQ(audio.track(a2)->clips[1].id, second);
}

TEST(AudioTracks, EdgesFollowTheRulesOfVideoPieces)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    // [1, 3) del file a 2 s, poi [0, 2) di un altro a 6 s, poi una terza a 9 s.
    const int a = audio.add(a2, clipOf("voce.wav", 1.0, 3.0, 2.0, 10.0));
    const int b = audio.add(a2, clipOf("musica.mp3", 0.0, 2.0, 6.0, 5.0));
    const int c = audio.add(a2, clipOf("effetto.wav", 0.0, 1.0, 9.0, 1.0));
    using Edge = Sequence::Edge;

    // Inizio di `a` indietro: non oltre l'inizio del file (il bordo nella sequenza va a 1 s).
    EXPECT_DOUBLE_EQ(audio.trimLimit(a, Edge::Start, -5.0, 0.1, false), 0.0);
    // Inizio di `b` indietro: si ferma contro la fine di `a` (5 s nella sequenza = 0 nel file, già al limite).
    EXPECT_DOUBLE_EQ(audio.trimLimit(b, Edge::Start, -1.0, 0.1, false), 0.0);
    // Fine di `a` avanti: contro `b` (6 s nella sequenza = 5 nel file).
    EXPECT_DOUBLE_EQ(audio.trimLimit(a, Edge::End, 9.0, 0.1, false), 5.0);
    // Fine di `b` avanti: `c` (a 9 s nella sequenza) la ferma a 3 s nel file, prima della fine del file (5 s).
    EXPECT_DOUBLE_EQ(audio.trimLimit(b, Edge::End, 9.0, 0.1, false), 3.0);
    EXPECT_DOUBLE_EQ(audio.trimLimit(b, Edge::End, 9.0, 0.1, true), 5.0);  // con ripple: fino alla fine del file
    // Non sotto la lunghezza minima.
    EXPECT_DOUBLE_EQ(audio.trimLimit(a, Edge::Start, 2.99, 0.1, false), 2.9);

    // Senza ripple: accorciando l'inizio la fine resta ferma e gli altri non si muovono.
    EXPECT_TRUE(audio.trim(a, Edge::Start, 1.5, 0.1, false));
    EXPECT_DOUBLE_EQ(audio.find(a)->position, 2.5);
    EXPECT_DOUBLE_EQ(audio.find(a)->endPosition(), 4.0);
    EXPECT_DOUBLE_EQ(audio.find(b)->position, 6.0);
    EXPECT_FALSE(audio.trim(a, Edge::Start, 1.5, 0.1, false));  // già lì

    // Con ripple: `b` si allunga di 1 s alla fine e `c` va avanti di 1 s.
    EXPECT_TRUE(audio.trim(b, Edge::End, 3.0, 0.1, true));
    EXPECT_DOUBLE_EQ(audio.find(b)->endPosition(), 9.0);
    EXPECT_DOUBLE_EQ(audio.find(c)->position, 10.0);
    // Con ripple dall'inizio: `b` resta dove inizia, le clip dopo scorrono indietro di quanto si accorcia.
    EXPECT_TRUE(audio.trim(b, Edge::Start, 0.5, 0.1, true));
    EXPECT_DOUBLE_EQ(audio.find(b)->position, 6.0);
    EXPECT_DOUBLE_EQ(audio.find(b)->endPosition(), 8.5);
    EXPECT_DOUBLE_EQ(audio.find(c)->position, 9.5);
    EXPECT_DOUBLE_EQ(audio.find(a)->position, 2.5);  // quella prima non si muove

    EXPECT_EQ(audio.trimLimit(999, Edge::End, 1.0, 0.1, false), -1.0);
}

TEST(AudioTracks, RemovingARangeLeavesSilence)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    const int a = audio.add(a2, clipOf("voce.wav", 0.0, 10.0, 0.0));
    const int b = audio.add(a2, clipOf("musica.mp3", 0.0, 2.0, 12.0));
    const int c = audio.add(a2, clipOf("effetto.wav", 0.0, 2.0, 15.0));

    // Nel mezzo di `a`: due clip, la seconda riparte da 6 nel file a 6 s.
    EXPECT_TRUE(audio.removeRange(a2, 4.0, 6.0));
    const auto& clips = audio.track(a2)->clips;
    ASSERT_EQ(clips.size(), 4u);
    EXPECT_EQ(clips[0].id, a);
    EXPECT_DOUBLE_EQ(clips[0].end, 4.0);
    EXPECT_DOUBLE_EQ(clips[1].start, 6.0);
    EXPECT_DOUBLE_EQ(clips[1].position, 6.0);
    EXPECT_NE(clips[1].id, a);

    // Su tutta `b` e l'inizio di `c`: `b` sparisce, `c` perde il primo mezzo secondo e resta dov'era il resto.
    EXPECT_TRUE(audio.removeRange(a2, 11.0, 15.5));
    EXPECT_EQ(audio.find(b), nullptr);
    ASSERT_NE(audio.find(c), nullptr);
    EXPECT_DOUBLE_EQ(audio.find(c)->start, 0.5);
    EXPECT_DOUBLE_EQ(audio.find(c)->position, 15.5);
    EXPECT_FALSE(audio.removeRange(a2, 20.0, 25.0));  // niente lì
    EXPECT_FALSE(audio.removeRange(a2, 5.0, 5.0));
}

TEST(AudioTracks, FilesCanBeRelocatedOrRemoved)
{
    AudioTracks audio;
    const int a2 = audio.addTrack();
    const int a3 = audio.addTrack();
    audio.add(a2, clipOf("voce.wav", 0.0, 1.0, 0.0));
    audio.add(a2, clipOf("voce.wav", 2.0, 3.0, 5.0));
    audio.add(a3, clipOf("musica.mp3", 0.0, 1.0, 0.0));
    EXPECT_EQ(audio.relocate("voce.wav", "altrove/voce.wav"), 2);
    EXPECT_EQ(audio.track(a2)->clips[1].path, "altrove/voce.wav");
    EXPECT_EQ(audio.removePath("musica.mp3"), 1);
    EXPECT_TRUE(audio.track(a3)->clips.empty());
}

// La traccia sostitutiva di prima è una clip su A2 con A1 silenziata.
TEST(AudioTracks, ReplacementIsAClipOnA2WithA1Silenced)
{
    Project project;
    EXPECT_FALSE(project.audioReplacement().has_value());
    project.setAudioReplacement(AudioReplacement{"musica.mp3", 1.5, 60.0});
    const AudioTracks& audio = project.audioTracks();
    EXPECT_FALSE(audio.audible(AudioTracks::kOriginalTrack));
    ASSERT_EQ(audio.tracks().size(), 1u);
    ASSERT_EQ(audio.tracks()[0].clips.size(), 1u);
    const AudioClip& clip = audio.tracks()[0].clips[0];
    EXPECT_EQ(audio.trackName(audio.tracks()[0].id), "A2");
    EXPECT_EQ(clip.path, "musica.mp3");
    EXPECT_DOUBLE_EQ(clip.position, 1.5);
    EXPECT_DOUBLE_EQ(clip.start, 0.0);
    EXPECT_DOUBLE_EQ(clip.end, 60.0);

    // Spostata prima dell'inizio: se ne salta l'inizio.
    project.setAudioOffset(-2.25);
    EXPECT_DOUBLE_EQ(project.audioTracks().tracks()[0].clips[0].position, 0.0);
    EXPECT_DOUBLE_EQ(project.audioTracks().tracks()[0].clips[0].start, 2.25);
    ASSERT_TRUE(project.audioReplacement().has_value());
    EXPECT_EQ(project.audioReplacement()->offset, -2.25);
    EXPECT_EQ(project.audioReplacement()->duration, 60.0);
    EXPECT_EQ(project.audioReplacement()->path, "musica.mp3");

    // Con una seconda clip non è più una semplice traccia sostitutiva.
    const int a2 = project.audioTracks().tracks()[0].id;
    const int extra = project.audioTracks().add(a2, clipOf("voce.wav", 0.0, 1.0, 70.0));
    ASSERT_NE(extra, 0);
    EXPECT_FALSE(project.audioReplacement().has_value());
    project.audioTracks().remove(extra);
    EXPECT_TRUE(project.audioReplacement().has_value());
    project.audioTracks().setLevel(a2, -3.0);  // nemmeno con il livello cambiato
    EXPECT_FALSE(project.audioReplacement().has_value());

    // Senza traccia sostitutiva si torna all'audio originale.
    project.setAudioReplacement(std::nullopt);
    EXPECT_TRUE(project.audioTracks().isDefault());
    EXPECT_FALSE(project.audioEdit().replacement.has_value());
}

TEST(AudioTracks, UndoAndRedoBringBackTracksAndClips)
{
    Project project;
    EditHistory history;
    history.record("Aggiungi traccia audio", project);
    const int a2 = project.audioTracks().addTrack();
    history.record("Aggiungi clip audio", project);
    const int clip = project.audioTracks().add(a2, clipOf("voce.wav", 0.0, 3.0, 1.0));
    history.record("Livello traccia", project);
    project.audioTracks().setLevel(a2, -9.0);
    const AudioTracks after = project.audioTracks();

    ASSERT_TRUE(history.undo(project));
    EXPECT_EQ(project.audioTracks().mix(a2).levelDb, 0.0);
    ASSERT_TRUE(history.undo(project));
    EXPECT_EQ(project.audioTracks().find(clip), nullptr);
    ASSERT_TRUE(history.undo(project));
    EXPECT_TRUE(project.audioTracks().isDefault());
    ASSERT_TRUE(history.redo(project));
    ASSERT_TRUE(history.redo(project));
    ASSERT_TRUE(history.redo(project));
    EXPECT_TRUE(project.audioTracks() == after);

    // Dopo un annulla, le clip e le tracce nuove non riprendono gli id di quelle tolte.
    ASSERT_TRUE(history.undo(project));
    ASSERT_TRUE(history.undo(project));
    const int again = project.audioTracks().add(a2, clipOf("musica.mp3", 0.0, 1.0, 0.0));
    EXPECT_NE(again, clip);
    EXPECT_NE(project.audioTracks().addTrack(), a2);
}

}  // namespace
