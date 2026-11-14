#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/Json.h"
#include "EditorCore/MixSpec.h"
#include "EditorCore/AudioMixer.h"
#include "EditorCore/Speed.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

Project oneFile(int& file)
{
    Project project;
    file = project.add("video.mp4");
    project.setAnalysis(file, {0.0}, 60.0);
    return project;
}

}  // namespace

TEST(Speed, MapConvertsBetweenFileAndSequenceTimes)
{
    const SpeedMap forward{2.0, 6.0, 2.0, false};  // 4 s di file a 2x = 2 s
    EXPECT_NEAR(forward.length(), 2.0, 1e-9);
    EXPECT_NEAR(forward.fileAt(1.0), 4.0, 1e-9);
    EXPECT_NEAR(forward.offsetOf(5.0), 1.5, 1e-9);
    const SpeedMap back{2.0, 6.0, 0.5, true};  // 8 s al contrario, a metà velocità
    EXPECT_NEAR(back.length(), 8.0, 1e-9);
    EXPECT_NEAR(back.fileAt(0.0), 6.0, 1e-9);
    EXPECT_NEAR(back.fileAt(2.0), 5.0, 1e-9);
    const TimeRange r = back.offsetsOf(3.0, 5.0);
    EXPECT_NEAR(r.start, 2.0, 1e-9);
    EXPECT_NEAR(r.end, 6.0, 1e-9);
    EXPECT_TRUE(clampSpeed(0.0) >= kMinSpeed);
    EXPECT_EQ(clampSpeed(100.0), kMaxSpeed);
}

TEST(Speed, FragmentLengthFollowsSpeedAndShiftsTheNextPieces)
{
    int file = 0;
    Project project = oneFile(file);
    Sequence& s = project.sequence();
    const int a = s.insert(-1, file, 0.0, 4.0, 2.0);   // 2 s nel montaggio
    const int b = s.insert(-1, file, 10.0, 12.0, 0.5);  // 4 s
    EXPECT_NEAR(s.find(a)->length(), 2.0, 1e-9);
    EXPECT_NEAR(s.find(b)->length(), 4.0, 1e-9);
    EXPECT_NEAR(s.startOf(1), 2.0, 1e-9);
    EXPECT_NEAR(s.totalLength(), 6.0, 1e-9);
    EXPECT_EQ(s.indexAt(1.9), 0);
    EXPECT_EQ(s.indexAt(2.1), 1);
    // Cambiare la velocità del primo (4x: 1 s) accorcia; con la spinta quelli dopo scorrono indietro.
    EXPECT_TRUE(s.setSpeed(a, 4.0, false, true));
    EXPECT_NEAR(s.totalLength(), 5.0, 1e-9);
    // Senza spinta resta uno spazio vuoto.
    EXPECT_TRUE(s.setSpeed(a, 8.0, false, false));
    EXPECT_NEAR(s.startOf(1), 1.0, 1e-9);  // 0.5 s di pezzo e 0.5 s di spazio vuoto
    EXPECT_NEAR(s.find(b)->gap, 0.5, 1e-9);
    EXPECT_FALSE(s.setSpeed(a, 8.0, false, false));  // non cambia nulla
}

TEST(Speed, SlowingDownPushesOrIsRefusedWithoutPush)
{
    int file = 0;
    Project project = oneFile(file);
    Sequence& s = project.sequence();
    const int a = s.insert(-1, file, 0.0, 2.0);
    const int b = s.insert(-1, file, 10.0, 12.0);
    EXPECT_FALSE(s.setSpeed(a, 0.5, false, false));  // 4 s, ma b è attaccato
    EXPECT_NEAR(s.find(a)->speed, 1.0, 1e-9);
    EXPECT_TRUE(s.setSpeed(a, 0.5, false, true));
    EXPECT_NEAR(s.startOf(s.indexOf(b)), 4.0, 1e-9);
}

TEST(Speed, SplitHonoursSpeedAndReverse)
{
    int file = 0;
    Project project = oneFile(file);
    Sequence& s = project.sequence();
    const int a = s.insert(-1, file, 10.0, 14.0, 2.0);  // 2 s: file 10..14
    const int second = s.split(0.5);                    // al file 11
    ASSERT_NE(second, 0);
    EXPECT_NEAR(s.find(a)->end, 11.0, 1e-9);
    EXPECT_NEAR(s.find(second)->start, 11.0, 1e-9);
    EXPECT_NEAR(s.find(second)->end, 14.0, 1e-9);
    EXPECT_NEAR(s.find(second)->speed, 2.0, 1e-9);

    const int r = s.insert(-1, file, 20.0, 24.0, 1.0, true);  // inverso: nel montaggio parte dal file 24
    const double at = s.startOf(s.indexOf(r));
    const int rs = s.split(at + 1.0);  // al file 23
    ASSERT_NE(rs, 0);
    // La parte che viene prima nel montaggio (r) è quella del file dopo il taglio.
    EXPECT_NEAR(s.find(r)->start, 23.0, 1e-9);
    EXPECT_NEAR(s.find(r)->end, 24.0, 1e-9);
    EXPECT_NEAR(s.find(rs)->start, 20.0, 1e-9);
    EXPECT_NEAR(s.find(rs)->end, 23.0, 1e-9);
    EXPECT_TRUE(s.find(rs)->reverse);
    EXPECT_LT(s.indexOf(r), s.indexOf(rs));
}

TEST(Speed, MutedRangesAreMappedToTheSequence)
{
    int file = 0;
    Project project = oneFile(file);
    Sequence& s = project.sequence();
    s.insert(-1, file, 10.0, 18.0, 2.0);  // 4 s
    EXPECT_TRUE(s.muteRange(1.0, 2.0));    // file 12..14
    const Fragment& f = s.fragments().front();
    ASSERT_EQ(f.muted.size(), 1u);
    EXPECT_NEAR(f.muted[0].start, 12.0, 1e-9);
    EXPECT_NEAR(f.muted[0].end, 14.0, 1e-9);
    const auto muted = s.mutedRanges();
    ASSERT_EQ(muted.size(), 1u);
    EXPECT_NEAR(muted[0].start, 1.0, 1e-9);
    EXPECT_NEAR(muted[0].end, 2.0, 1e-9);

    Sequence r;
    r.insert(-1, file, 10.0, 18.0, 2.0, true);
    EXPECT_TRUE(r.muteRange(0.0, 1.0));  // all'inizio del montaggio = fine del file
    EXPECT_NEAR(r.fragments().front().muted[0].start, 16.0, 1e-9);
    EXPECT_NEAR(r.fragments().front().muted[0].end, 18.0, 1e-9);
    EXPECT_NEAR(r.mutedRanges()[0].end, 1.0, 1e-9);
}

TEST(Speed, TrimOfEdgesConvertsFileToSequenceTime)
{
    int file = 0;
    Project project = oneFile(file);
    Sequence& s = project.sequence();
    const int a = s.insert(-1, file, 10.0, 20.0, 2.0);  // 5 s
    const int b = s.insert(-1, file, 30.0, 32.0);
    // Accorciare la fine di 2 s di file = 1 s nel montaggio: il pezzo dopo resta fermo (spazio vuoto).
    EXPECT_TRUE(s.trim(a, Sequence::Edge::End, 18.0, 60.0, 0.1, false));
    EXPECT_NEAR(s.find(a)->length(), 4.0, 1e-9);
    EXPECT_NEAR(s.find(b)->gap, 1.0, 1e-9);
    // Allungare l'inizio all'indietro oltre lo spazio disponibile (0 s) non si può.
    EXPECT_NEAR(s.trimLimit(a, Sequence::Edge::Start, 5.0, 60.0, 0.1, false), 10.0, 1e-9);
    // Con l'inverso la fine del file sta a sinistra: allungarla riempie lo spazio prima.
    Sequence r;
    r.insert(-1, file, 10.0, 20.0, 2.0, true);
    r.insert(-1, file, 30.0, 32.0, 1.0, false);
    r.moveTo(r.fragments()[0].id, 3.0);  // 3 s di spazio prima
    const int id = r.fragments()[0].id;
    EXPECT_NEAR(r.trimLimit(id, Sequence::Edge::End, 100.0, 60.0, 0.1, false), 26.0, 1e-9);  // 3 s * 2
    EXPECT_TRUE(r.trim(id, Sequence::Edge::End, 24.0, 60.0, 0.1, false));
    EXPECT_NEAR(r.fragments()[0].gap, 1.0, 1e-9);
    EXPECT_NEAR(r.fragments()[0].length(), 7.0, 1e-9);
}

TEST(Speed, AudioClipsSplitTrimAndChangeSpeed)
{
    AudioTracks tracks;
    const int track = tracks.addTrack();
    AudioClip clip;
    clip.path = "a.mp3";
    clip.start = 10.0;
    clip.end = 14.0;
    clip.speed = 2.0;
    clip.position = 1.0;
    clip.sourceDuration = 100.0;
    const int id = tracks.add(track, clip);
    ASSERT_NE(id, 0);
    EXPECT_NEAR(tracks.find(id)->endPosition(), 3.0, 1e-9);
    const int second = tracks.split(track, 2.0);
    ASSERT_NE(second, 0);
    EXPECT_NEAR(tracks.find(id)->end, 12.0, 1e-9);
    EXPECT_NEAR(tracks.find(second)->start, 12.0, 1e-9);
    EXPECT_NEAR(tracks.find(second)->position, 2.0, 1e-9);
    EXPECT_TRUE(tracks.removeRange(track, 1.5, 2.5));
    EXPECT_NEAR(tracks.find(id)->end, 11.0, 1e-9);
    EXPECT_NEAR(tracks.find(second)->start, 13.0, 1e-9);
    // Rallentando la prima parte (0.5x: 2 s di file = 4 s) si sovrappone alla seconda: rifiutato.
    EXPECT_FALSE(tracks.setSpeed(id, 0.5, false));
    EXPECT_TRUE(tracks.setSpeed(id, 4.0, true));
    EXPECT_TRUE(tracks.find(id)->reverse);
}

TEST(Speed, LayerPiecesSplitAndKeepTheirSpeed)
{
    int file = 0;
    Project project = oneFile(file);
    const int layer = project.textTracks().addLayer();
    LayerPiece piece;
    piece.clipId = file;
    piece.start = 10.0;
    piece.end = 14.0;
    piece.speed = 2.0;
    piece.position = 1.0;
    const int id = project.textTracks().addPiece(layer, piece);
    ASSERT_NE(id, 0);
    EXPECT_NEAR(project.textTracks().findPiece(id)->endPosition(), 3.0, 1e-9);
    const int second = project.textTracks().splitPiece(id, 2.0);
    ASSERT_NE(second, 0);
    EXPECT_NEAR(project.textTracks().findPiece(id)->end, 12.0, 1e-9);
    EXPECT_NEAR(project.textTracks().findPiece(second)->start, 12.0, 1e-9);
    EXPECT_NEAR(project.textTracks().findPiece(second)->speed, 2.0, 1e-9);
    EXPECT_FALSE(project.textTracks().setPieceSpeed(id, 0.5, false));  // si sovrappone alla seconda parte
    EXPECT_TRUE(project.textTracks().setPieceSpeed(second, 4.0, true));
}

TEST(Speed, UnlinkAndRelinkAudioKeepSpeed)
{
    int file = 0;
    Project project = oneFile(file);
    const int id = project.sequence().insert(-1, file, 10.0, 14.0, 2.0, true);
    project.sequence().muteRange(0.5, 1.0);
    int track = 0;
    const int clip = project.unlinkAudio(id, &track);
    ASSERT_NE(clip, 0);
    const AudioClip* c = project.audioTracks().find(clip);
    EXPECT_NEAR(c->speed, 2.0, 1e-9);
    EXPECT_TRUE(c->reverse);
    EXPECT_TRUE(project.relinkAudio(id));
}

TEST(Speed, SavesAndReloadsAndOldFilesLoadAtNormalSpeed)
{
    int file = 0;
    Project project = oneFile(file);
    ProjectView view;
    const int plainId = project.sequence().insert(-1, file, 0.0, 2.0);
    const std::string plain = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(plain.find("velocita"), std::string::npos);
    EXPECT_EQ(plain.find("inverso"), std::string::npos);
    (void)plainId;

    const int fast = project.sequence().insert(-1, file, 10.0, 14.0, 2.0, true);
    const int layer = project.textTracks().addLayer();
    LayerPiece piece;
    piece.clipId = file;
    piece.start = 1.0;
    piece.end = 3.0;
    piece.speed = 0.25;
    piece.position = 0.0;
    ASSERT_NE(project.textTracks().addPiece(layer, piece), 0);
    AudioClip clip;
    clip.path = "musica.mp3";
    clip.end = 8.0;
    clip.speed = 4.0;
    clip.position = 0.0;
    clip.sourceDuration = 20.0;
    ASSERT_NE(project.audioTracks().add(project.audioTracks().addTrack(), clip), 0);
    (void)fast;

    const std::string json = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(parseJson(json)["versione"].asInteger(), kProjectFormatVersion);
    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    const auto& fragments = loaded.project.sequence().fragments();
    ASSERT_EQ(fragments.size(), 2u);
    EXPECT_NEAR(fragments[0].speed, 1.0, 1e-9);
    EXPECT_FALSE(fragments[0].reverse);
    EXPECT_NEAR(fragments[1].speed, 2.0, 1e-9);
    EXPECT_TRUE(fragments[1].reverse);
    ASSERT_EQ(loaded.project.textTracks().layers().size(), 1u);
    EXPECT_NEAR(loaded.project.textTracks().layers()[0].pieces[0].speed, 0.25, 1e-9);
    bool found = false;
    for (const AudioTrack& t : loaded.project.audioTracks().tracks()) {
        for (const AudioClip& c : t.clips) {
            if (c.path == "musica.mp3") {
                EXPECT_NEAR(c.speed, 4.0, 1e-9);
                found = true;
            }
        }
    }
    EXPECT_TRUE(found);
}

TEST(Speed, MixSegmentsCarrySpeedAndSequenceLength)
{
    int file = 0;
    Project project = oneFile(file);
    project.sequence().insert(-1, file, 10.0, 14.0, 2.0, true);
    const std::vector<MixSegment> segments = mixSegmentsOf(project);
    ASSERT_EQ(segments.size(), 1u);
    EXPECT_NEAR(segments[0].speed, 2.0, 1e-9);
    EXPECT_TRUE(segments[0].reverse);
    EXPECT_NEAR(segments[0].length(), 2.0, 1e-9);
    EXPECT_NEAR(segments[0].fileLength(), 4.0, 1e-9);
}

TEST(Speed, ProjectSetSpeedChangesGroupsTogether)
{
    int file = 0;
    Project project = oneFile(file);
    const int piece = project.sequence().insert(-1, file, 0.0, 4.0);
    const int next = project.sequence().insert(-1, file, 10.0, 12.0);
    // 2x sul primo pezzo: dura 2 s, il successivo scorre indietro (spinta attiva) e l'audio collegato segue.
    const Project::SpeedResult r = project.setSpeed({{ObjectRef::Kind::Piece, piece}}, 2.0, std::nullopt, true);
    EXPECT_EQ(r.changed, 1);
    EXPECT_FALSE(r.blocked);
    EXPECT_NEAR(project.sequence().find(piece)->length(), 2.0, 1e-9);
    EXPECT_NEAR(project.sequence().startOf(project.sequence().indexOf(next)), 2.0 + 0.0, 1e-9);
    // Solo l'inverso: la velocità resta.
    EXPECT_EQ(project.setSpeed({{ObjectRef::Kind::PieceAudio, piece}}, std::nullopt, true, true).changed, 1);
    EXPECT_TRUE(project.sequence().find(piece)->reverse);
    EXPECT_NEAR(project.sequence().find(piece)->speed, 2.0, 1e-9);
    // Gli stessi valori: niente da fare.
    EXPECT_EQ(project.setSpeed({{ObjectRef::Kind::Piece, piece}}, 2.0, true, true).changed, 0);
}

TEST(Speed, ProjectSetSpeedFollowsTheGroup)
{
    int file = 0;
    Project project = oneFile(file);
    const int layer = project.textTracks().addLayer();
    LayerPiece piece;
    piece.clipId = file;
    piece.start = 0.0;
    piece.end = 4.0;
    piece.position = 0.0;
    int audio = 0;
    const int id = project.addLayerPiece(layer, piece, true, &audio);
    ASSERT_NE(id, 0);
    ASSERT_NE(audio, 0);
    // Cambiando il pezzo cambia anche la clip audio del suo gruppo.
    EXPECT_EQ(project.setSpeed({{ObjectRef::Kind::LayerPiece, id}}, 2.0, std::nullopt, true).changed, 2);
    EXPECT_NEAR(project.audioTracks().find(audio)->speed, 2.0, 1e-9);
    EXPECT_NEAR(project.textTracks().findPiece(id)->length(), 2.0, 1e-9);
}

TEST(Speed, ProjectSetSpeedRefusesWhatDoesNotFitAndChangesNothing)
{
    int file = 0;
    Project project = oneFile(file);
    const int a = project.sequence().insert(-1, file, 0.0, 2.0);
    const int b = project.sequence().insert(-1, file, 10.0, 12.0);
    (void)b;
    // 0.5x sul primo senza spinta: 4 s, ma il secondo è attaccato.
    const Project::SpeedResult r = project.setSpeed({{ObjectRef::Kind::Piece, a}}, 0.5, std::nullopt, false);
    EXPECT_TRUE(r.blocked);
    EXPECT_EQ(r.changed, 0);
    EXPECT_NEAR(project.sequence().find(a)->speed, 1.0, 1e-9);
}

TEST(Speed, StillImagesHaveNoSpeed)
{
    Project project;
    const int image = project.add("foto.png");
    project.setAnalysis(image, {0.0}, 5.0);
    const int piece = project.sequence().insert(-1, image, 0.0, 5.0);
    EXPECT_EQ(project.setSpeed({{ObjectRef::Kind::Piece, piece}}, 2.0, std::nullopt, true).changed, 0);
}

TEST(Speed, ASpedPieceMakesTheAudioNeedTheMix)
{
    int file = 0;
    Project project = oneFile(file);
    const int a = project.sequence().insert(-1, file, 0.0, 2.0);
    EXPECT_FALSE(project.audioEdit(GapExport::Black).mix);  // come prima: l'audio si copia
    ASSERT_TRUE(project.sequence().setSpeed(a, 2.0, false, true));
    const AudioEdit edit = project.audioEdit(GapExport::Black);
    ASSERT_TRUE(edit.mix);
    ASSERT_EQ(edit.mix->segments.size(), 1u);
    EXPECT_NEAR(edit.mix->segments[0].length(), 1.0, 1e-9);
    EXPECT_NEAR(edit.mixClosed->segments[0].length(), 1.0, 1e-9);
}

TEST(Speed, AudioOptionsDefaultToKeepPitchAndSoundWhenReversed)
{
    int file = 0;
    Project project = oneFile(file);
    const int id = project.sequence().insert(-1, file, 10.0, 14.0, 2.0, true);
    EXPECT_TRUE(project.sequence().find(id)->audioSpeed.keepPitch);
    EXPECT_FALSE(project.sequence().find(id)->audioSpeed.muteWhenReversed);
    const std::string json = projectToJson("C:/prova/montaggio.evproj", project, ProjectView{});
    EXPECT_EQ(json.find("intonazioneLibera"), std::string::npos);  // i progetti normali non cambiano
    EXPECT_EQ(json.find("silenzioInverso"), std::string::npos);
}

TEST(Speed, AudioOptionsChangeTogetherWithTheGroupAndSurviveSplitSaveAndUnlink)
{
    int file = 0;
    Project project = oneFile(file);
    const int id = project.sequence().insert(-1, file, 10.0, 18.0, 2.0, true);
    EXPECT_EQ(project.setAudioSpeedOptions({{ObjectRef::Kind::Piece, id}}, false, true), 1);
    EXPECT_EQ(project.setAudioSpeedOptions({{ObjectRef::Kind::Piece, id}}, false, true), 0);  // già così
    const Fragment* f = project.sequence().find(id);
    EXPECT_FALSE(f->audioSpeed.keepPitch);
    EXPECT_TRUE(f->audioSpeed.muteWhenReversed);
    // L'audio nel mix porta le due scelte.
    const std::vector<MixSegment> segments = mixSegmentsOf(project);
    ASSERT_EQ(segments.size(), 1u);
    EXPECT_FALSE(segments[0].audioSpeed.keepPitch);
    EXPECT_TRUE(segments[0].audioSpeed.muteWhenReversed);
    // Dividere il pezzo le tiene in tutte e due le parti.
    const int second = project.sequence().split(1.0);  // un secondo dopo l'inizio del montaggio
    ASSERT_NE(second, 0);
    EXPECT_EQ(project.sequence().find(second)->audioSpeed, project.sequence().find(id)->audioSpeed);
    // Si salvano e si rileggono.
    const std::string json = projectToJson("C:/prova/montaggio.evproj", project, ProjectView{});
    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    for (const Fragment& g : loaded.project.sequence().fragments()) {
        EXPECT_FALSE(g.audioSpeed.keepPitch);
        EXPECT_TRUE(g.audioSpeed.muteWhenReversed);
    }
    // Scollegare l'audio e ricollegarlo le conserva.
    int track = 0;
    const int clip = project.unlinkAudio(id, &track);
    ASSERT_NE(clip, 0);
    EXPECT_FALSE(project.audioTracks().find(clip)->audioSpeed.keepPitch);
    EXPECT_TRUE(project.audioTracks().find(clip)->audioSpeed.muteWhenReversed);
    EXPECT_EQ(project.setAudioSpeedOptions({{ObjectRef::Kind::AudioClip, clip}}, true, std::nullopt), 1);
    EXPECT_TRUE(project.audioTracks().find(clip)->audioSpeed.keepPitch);
    EXPECT_TRUE(project.audioTracks().find(clip)->audioSpeed.muteWhenReversed);
}
