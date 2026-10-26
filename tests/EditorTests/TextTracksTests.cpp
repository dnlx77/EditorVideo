#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/TextTracks.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace editor::core;

namespace {

TextClip title(double position, double length, const char* text = "Titolo")
{
    TextClip clip;
    clip.position = position;
    clip.length = length;
    clip.text = text;
    return clip;
}

}  // namespace

TEST(TextTracks, LayersAreNamedByPosition)
{
    TextTracks t;
    EXPECT_TRUE(t.empty());
    const int a = t.addLayer();
    const int b = t.addLayer();
    EXPECT_EQ(t.layerName(a), "T1");
    EXPECT_EQ(t.layerName(b), "T2");
    EXPECT_TRUE(t.removeLayer(a));
    EXPECT_EQ(t.layerName(b), "T1");
    EXPECT_TRUE(t.layerName(a).empty());
}

TEST(TextTracks, ClipsOnTheSameLayerNeverOverlap)
{
    TextTracks t;
    const int layer = t.addLayer();
    const int first = t.add(layer, title(2.0, 3.0));
    ASSERT_NE(first, 0);
    EXPECT_EQ(t.add(layer, title(4.0, 2.0)), 0);  // dentro la prima
    EXPECT_EQ(t.add(layer, title(0.0, 2.5)), 0);  // la tocca da prima
    EXPECT_NE(t.add(layer, title(0.0, 2.0)), 0);  // attaccata: ok
    EXPECT_NE(t.add(layer, title(5.0, 1.0)), 0);
    EXPECT_EQ(t.add(layer, title(-1.0, 0.5)), 0);
    EXPECT_EQ(t.add(layer, title(20.0, 0.01)), 0);  // troppo corta
    EXPECT_EQ(t.add(99, title(20.0, 1.0)), 0);      // livello inesistente
    const auto& clips = t.layer(layer)->clips;
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_LT(clips[0].position, clips[1].position);  // in ordine di posizione
    EXPECT_LT(clips[1].position, clips[2].position);
}

TEST(TextTracks, OverlapAcrossLayersIsAllowed)
{
    TextTracks t;
    const int l1 = t.addLayer();
    const int l2 = t.addLayer();
    const int a = t.add(l1, title(0.0, 5.0, "sotto"));
    const int b = t.add(l2, title(2.0, 5.0, "sopra"));
    const auto visible = t.visibleAt(3.0);
    ASSERT_EQ(visible.size(), 2u);
    EXPECT_EQ(visible[0]->id, a);  // dal livello più basso al più alto
    EXPECT_EQ(visible[1]->id, b);
    EXPECT_EQ(t.visibleAt(6.0).size(), 1u);
    EXPECT_TRUE(t.visibleAt(7.0).empty());  // la fine non è inclusa
    EXPECT_DOUBLE_EQ(t.endTime(), 7.0);
}

TEST(TextTracks, MovingStopsAtNeighboursAndCanChangeLayer)
{
    TextTracks t;
    const int l1 = t.addLayer();
    const int l2 = t.addLayer();
    const int a = t.add(l1, title(0.0, 2.0));
    const int b = t.add(l1, title(5.0, 2.0));
    EXPECT_FALSE(t.moveTo(a, l1, 4.0));  // urterebbe b
    EXPECT_DOUBLE_EQ(t.find(a)->position, 0.0);
    EXPECT_DOUBLE_EQ(t.nearestFreeStart(a, l1, 4.0), 3.0);  // contro b
    EXPECT_DOUBLE_EQ(t.nearestFreeStart(a, l1, 6.0), 7.0);  // dopo b è più vicino
    EXPECT_DOUBLE_EQ(t.nearestFreeStart(a, l1, 2.5), 2.5);
    EXPECT_TRUE(t.moveTo(a, l1, 2.5));
    EXPECT_TRUE(t.moveTo(a, l2, 4.0));  // su un altro livello può sovrapporsi in tempo
    EXPECT_EQ(t.layerOf(a), l2);
    EXPECT_EQ(t.layerOf(b), l1);
    EXPECT_DOUBLE_EQ(t.find(a)->position, 4.0);
    EXPECT_EQ(t.find(a)->id, a);  // l'id non cambia
    EXPECT_DOUBLE_EQ(t.nearestFreeStart(a, 99, 1.0), -1.0);
}

TEST(TextTracks, EdgesTrimWithinLimits)
{
    TextTracks t;
    const int layer = t.addLayer();
    const int a = t.add(layer, title(1.0, 4.0));
    const int b = t.add(layer, title(7.0, 1.0));
    EXPECT_TRUE(t.setStart(a, 2.0));
    EXPECT_DOUBLE_EQ(t.find(a)->length, 3.0);  // la fine resta a 5
    EXPECT_FALSE(t.setStart(a, 4.95));         // sotto la durata minima
    EXPECT_TRUE(t.setEnd(a, 6.5));
    EXPECT_FALSE(t.setEnd(a, 7.5));  // urta b
    EXPECT_FALSE(t.setStart(b, -1.0));
    EXPECT_TRUE(t.setLength(b, 3.0));
    EXPECT_DOUBLE_EQ(t.find(b)->endPosition(), 10.0);
}

TEST(TextTracks, SplitKeepsTextAndStyle)
{
    TextTracks t;
    const int layer = t.addLayer();
    TextClip clip = title(1.0, 4.0, "Ciao\nmondo");
    clip.style.bold = true;
    clip.style.color = {10, 20, 30, 255};
    const int a = t.add(layer, clip);
    EXPECT_EQ(t.split(a, 1.05), 0);  // parte troppo corta
    const int second = t.split(a, 3.0);
    ASSERT_NE(second, 0);
    EXPECT_DOUBLE_EQ(t.find(a)->length, 2.0);
    EXPECT_DOUBLE_EQ(t.find(second)->position, 3.0);
    EXPECT_DOUBLE_EQ(t.find(second)->length, 2.0);
    EXPECT_EQ(t.find(second)->text, "Ciao\nmondo");
    EXPECT_EQ(t.find(second)->style, clip.style);
    EXPECT_EQ(t.layerOf(second), layer);
}

TEST(TextTracks, TextAndStyleEditsReportChanges)
{
    TextTracks t;
    const int a = t.add(t.addLayer(), title(0.0, 2.0));
    EXPECT_FALSE(t.setText(a, "Titolo"));  // uguale
    EXPECT_TRUE(t.setText(a, "Altro"));
    TextStyle style = t.find(a)->style;
    EXPECT_FALSE(t.setStyle(a, style));
    style.fontFamily = "Arial";
    style.size = 0.1;
    EXPECT_TRUE(t.setStyle(a, style));
    EXPECT_EQ(t.find(a)->style.fontFamily, "Arial");
    EXPECT_FALSE(t.setText(999, "x"));
}

TEST(TextTracks, UndoRestoreDoesNotReuseIds)
{
    Project project;
    const int layer = project.textTracks().addLayer();
    const Project before = project;
    const int a = project.textTracks().add(layer, title(0.0, 2.0));
    project.restoreEdits(before);
    EXPECT_TRUE(project.textTracks().empty());
    const int b = project.textTracks().add(layer, title(0.0, 2.0));
    EXPECT_NE(a, b);
    project.clear();
    EXPECT_TRUE(project.textTracks().layers().empty());
}

TEST(TextTracks, SurviveSaveAndReopen)
{
    Project project;
    const int l1 = project.textTracks().addLayer();
    const int l2 = project.textTracks().addLayer();
    TextClip clip = title(1.5, 4.25, "Città\n\"vecchia\" 🎬");
    clip.style.fontFamily = "Courier New";
    clip.style.size = 0.12;
    clip.style.bold = clip.style.italic = clip.style.underline = true;
    clip.style.align = TextAlign::Right;
    clip.style.lineSpacing = 1.4;
    clip.style.color = {200, 100, 50, 255};
    clip.style.opacity = 0.75;
    clip.style.outline = true;
    clip.style.outlineColor = {1, 2, 3, 4};
    clip.style.shadow = true;
    clip.style.background = true;
    clip.style.backgroundColor = {9, 8, 7, 128};
    clip.style.centerX = 0.25;
    clip.style.centerY = 0.1;
    project.textTracks().add(l1, clip);
    project.textTracks().add(l1, title(10.0, 2.0, "Seconda"));
    project.textTracks().add(l2, title(2.0, 1.0, "Sopra"));

    const std::string json = projectToJson("C:/prova/p.evproj", project, ProjectView{});
    const LoadedProject loaded = projectFromJson("C:/prova/p.evproj", json);
    const TextTracks& back = loaded.project.textTracks();
    ASSERT_EQ(back.layers().size(), 2u);
    ASSERT_EQ(back.layers()[0].clips.size(), 2u);
    ASSERT_EQ(back.layers()[1].clips.size(), 1u);
    // Gli id cambiano riaprendo, il resto è identico.
    TextClip expected = clip;
    TextClip got = back.layers()[0].clips[0];
    expected.id = got.id = 0;
    EXPECT_EQ(got, expected);
    EXPECT_EQ(back.layers()[0].clips[1].text, "Seconda");
    EXPECT_EQ(back.layers()[1].clips[0].text, "Sopra");
    EXPECT_DOUBLE_EQ(back.layers()[1].clips[0].position, 2.0);
}

TEST(TextTracks, ProjectWithoutTextKeepsOldFormatVersion)
{
    Project project;
    const std::string json = projectToJson("C:/prova/p.evproj", project, ProjectView{});
    EXPECT_EQ(json.find("scritte"), std::string::npos);
    EXPECT_NE(json.find("\"versione\": 1"), std::string::npos) << json;
    // Un livello vuoto si salva comunque: la versione sale a 4.
    project.textTracks().addLayer();
    const std::string withLayer = projectToJson("C:/prova/p.evproj", project, ProjectView{});
    EXPECT_NE(withLayer.find("\"versione\": 4"), std::string::npos);
    EXPECT_EQ(projectFromJson("C:/prova/p.evproj", withLayer).project.textTracks().layers().size(), 1u);
}

TEST(TextTracks, BrokenTextClipIsRejected)
{
    Project project;
    project.textTracks().add(project.textTracks().addLayer(), title(0.0, 2.0));
    std::string json = projectToJson("C:/prova/p.evproj", project, ProjectView{});
    const std::size_t at = json.find("\"durata\"");
    ASSERT_NE(at, std::string::npos);
    const std::size_t colon = json.find(':', at);
    const std::size_t end = json.find_first_of(",}", colon);
    json.replace(colon + 1, end - colon - 1, "0");
    EXPECT_THROW(projectFromJson("C:/prova/p.evproj", json), ProjectFileError);
}

// ---------------------------------------------------------------------------------------------------------------
// Limiti dei bordi e posti liberi

TEST(TextTracks, EdgeLimitsStopAtNeighboursAndMinimumLength)
{
    TextTracks t;
    const int layer = t.addLayer();
    t.add(layer, title(0.0, 2.0));
    const int mid = t.add(layer, title(4.0, 2.0));
    t.add(layer, title(9.0, 1.0));
    EXPECT_DOUBLE_EQ(t.edgeLimit(mid, true, 0.0), 2.0);   // contro la scritta prima
    EXPECT_DOUBLE_EQ(t.edgeLimit(mid, true, 5.99), 5.9);  // lunga almeno kMinLength
    EXPECT_DOUBLE_EQ(t.edgeLimit(mid, true, 3.0), 3.0);
    EXPECT_DOUBLE_EQ(t.edgeLimit(mid, false, 20.0), 9.0);  // contro quella dopo
    EXPECT_DOUBLE_EQ(t.edgeLimit(mid, false, 4.0), 4.1);
    EXPECT_DOUBLE_EQ(t.edgeLimit(999, true, 1.0), -1.0);
    EXPECT_TRUE(t.canPlace(layer, 2.0, 2.0));
    EXPECT_FALSE(t.canPlace(layer, 1.9, 2.0));
    EXPECT_FALSE(t.canPlace(layer, -0.5, 1.0));
    EXPECT_FALSE(t.canPlace(99, 20.0, 1.0));
}

// ---------------------------------------------------------------------------------------------------------------
// Scritte nei gruppi

namespace {

using Kind = ObjectRef::Kind;

struct TextMontage
{
    Project project;
    int piece = 0;  // un pezzo di 4 s: 0 .. 4
    int layer1 = 0;
    int layer2 = 0;
    int a = 0;  // scritta 1 .. 3 sul livello 1
    int b = 0;  // scritta 6 .. 8 sul livello 1
    int c = 0;  // scritta 2 .. 4 sul livello 2

    TextMontage()
    {
        const int file = project.add("video.mp4");
        project.setAnalysis(file, {0.0}, 30.0);
        piece = project.sequence().insert(-1, file, 0.0, 4.0);
        layer1 = project.textTracks().addLayer();
        layer2 = project.textTracks().addLayer();
        a = project.textTracks().add(layer1, title(1.0, 2.0, "A"));
        b = project.textTracks().add(layer1, title(6.0, 2.0, "B"));
        c = project.textTracks().add(layer2, title(2.0, 2.0, "C"));
    }
};

}  // namespace

TEST(TextGroups, ExistAndGroupWithPiecesAndAudio)
{
    TextMontage m;
    EXPECT_EQ(m.project.groupMembers({Kind::TextClip, m.a}).size(), 1u);
    EXPECT_TRUE(m.project.groupMembers({Kind::TextClip, 999}).empty());
    const int id = m.project.groupObjects({{Kind::Piece, m.piece}, {Kind::TextClip, m.a}});
    ASSERT_NE(id, 0);
    // Il gruppo porta con sé anche l'audio del pezzo; gli id della scritta e del pezzo possono coincidere senza confusione.
    const auto members = m.project.groupMembers({Kind::TextClip, m.a});
    EXPECT_EQ(members.size(), 3u);
    EXPECT_TRUE(m.project.groupMembers({Kind::TextClip, m.b}).size() == 1u);
}

TEST(TextGroups, MoveTogetherAndStopAgainstNeighbours)
{
    TextMontage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.piece}, {Kind::TextClip, m.a}}), 0);
    // La scritta A (1..3) e il pezzo (0..4) si muovono insieme; B (6..8) sullo stesso livello blocca a +3.
    EXPECT_DOUBLE_EQ(m.project.clampGroupDelta({Kind::TextClip, m.a}, 10.0, false), 3.0);
    EXPECT_DOUBLE_EQ(m.project.clampGroupDelta({Kind::Piece, m.piece}, -5.0, false), 0.0);  // il pezzo è già a 0
    const auto moved = m.project.moveObject({Kind::TextClip, m.a}, m.layer1, 3.0, false);  // +2
    EXPECT_TRUE(moved.moved);
    EXPECT_TRUE(moved.group);
    EXPECT_DOUBLE_EQ(m.project.textTracks().find(m.a)->position, 3.0);
    EXPECT_DOUBLE_EQ(m.project.sequence().startOf(m.project.sequence().indexOf(m.piece)), 2.0);
    // Senza spinta le scritte degli altri non si toccano.
    EXPECT_DOUBLE_EQ(m.project.textTracks().find(m.b)->position, 6.0);
    // Con la spinta, come per le clip audio, quelle urtate scorrono davanti.
    EXPECT_DOUBLE_EQ(m.project.clampGroupDelta({Kind::TextClip, m.a}, 10.0, true), 10.0);
    EXPECT_TRUE(m.project.moveObject({Kind::TextClip, m.a}, m.layer1, 6.0, true).moved);  // +3: urta B (6..8)
    EXPECT_DOUBLE_EQ(m.project.textTracks().find(m.a)->position, 6.0);
    EXPECT_DOUBLE_EQ(m.project.textTracks().find(m.b)->position, 8.0);
}

TEST(TextGroups, SingleTextClipMovesWithinItsLayerOrToAnother)
{
    TextMontage m;
    EXPECT_DOUBLE_EQ(m.project.freeSpotFor({Kind::TextClip, m.a}, m.layer1, 5.0, true), 4.0);  // contro B
    EXPECT_DOUBLE_EQ(m.project.freeSpotFor({Kind::TextClip, m.a}, m.layer2, 3.0, true), 4.0);  // contro C (2..4)
    EXPECT_DOUBLE_EQ(m.project.freeSpotFor({Kind::TextClip, 999}, m.layer1, 3.0, true), -1.0);
    const auto r = m.project.moveObject({Kind::TextClip, m.a}, m.layer2, 5.0, true);
    EXPECT_TRUE(r.moved);
    EXPECT_FALSE(r.group);
    EXPECT_EQ(m.project.textTracks().layerOf(m.a), m.layer2);
    EXPECT_FALSE(m.project.moveObject({Kind::TextClip, m.b}, m.layer2, 5.5, true).moved);  // sopra A (5..7 sul livello 2)
}

TEST(TextGroups, SplitAndRemoveGroupMembers)
{
    TextMontage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.piece}, {Kind::TextClip, m.a}, {Kind::TextClip, m.c}}), 0);
    const auto parts = m.project.splitGroup({Kind::Piece, m.piece}, 2.5);  // taglia pezzo, A (1..3) e C (2..4)
    int pieces = 0;
    int texts = 0;
    for (const ObjectRef& part : parts) {
        pieces += part.kind == Kind::Piece;
        texts += part.kind == Kind::TextClip;
    }
    EXPECT_EQ(pieces, 1);
    EXPECT_EQ(texts, 2);
    EXPECT_DOUBLE_EQ(m.project.textTracks().find(m.a)->length, 1.5);
    EXPECT_DOUBLE_EQ(m.project.textTracks().find(m.c)->length, 0.5);
    // Le seconde parti formano un gruppo nuovo.
    for (const ObjectRef& part : parts) {
        if (part.kind == Kind::TextClip)
            EXPECT_GE(m.project.groupMembers(part).size(), 3u);
    }
    // Si tolgono solo gli oggetti scelti.
    EXPECT_EQ(m.project.removeObjects({{Kind::TextClip, m.a}}), 1);
    EXPECT_EQ(m.project.textTracks().find(m.a), nullptr);
    EXPECT_NE(m.project.textTracks().find(m.c), nullptr);
    // Il gruppo perde l'oggetto tolto ma resta valido.
    for (const auto& group : m.project.validGroups())
        EXPECT_GE(group.size(), 2u);
}

TEST(TextGroups, UngroupDissolvesWithoutTouchingTheAudioOfPieces)
{
    TextMontage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.piece}, {Kind::TextClip, m.a}}), 0);
    const auto result = m.project.ungroupObjects({{Kind::TextClip, m.a}});
    EXPECT_EQ(result.dissolved, 1);
    EXPECT_EQ(result.audioUnlinked, 0);
    // Una scritta da sola non scollega l'audio di un pezzo che ha lo stesso id.
    const auto alone = m.project.ungroupObjects({{Kind::TextClip, m.b}});
    EXPECT_EQ(alone.dissolved, 0);
    EXPECT_EQ(alone.audioUnlinked, 0);
    EXPECT_FALSE(m.project.sequence().find(m.piece)->detached);
}

TEST(TextGroups, SurviveSaveAndReopen)
{
    TextMontage m;
    ASSERT_NE(m.project.groupObjects({{Kind::Piece, m.piece}, {Kind::TextClip, m.b}, {Kind::TextClip, m.c}}), 0);
    const std::string json = projectToJson("C:/prova/p.evproj", m.project, ProjectView{});
    const LoadedProject loaded = projectFromJson("C:/prova/p.evproj", json);
    const TextTracks& text = loaded.project.textTracks();
    ASSERT_EQ(text.layers().size(), 2u);
    const int b = text.layers()[0].clips[1].id;  // B è la seconda del primo livello
    const int c = text.layers()[1].clips[0].id;
    EXPECT_EQ(text.find(b)->text, "B");
    EXPECT_EQ(text.find(c)->text, "C");
    const auto members = loaded.project.groupMembers({Kind::TextClip, b});
    EXPECT_TRUE(std::find(members.begin(), members.end(), ObjectRef{Kind::TextClip, c}) != members.end());
    EXPECT_EQ(members.size(), 4u);  // pezzo, audio del pezzo, B, C
    EXPECT_EQ(loaded.project.groupMembers({Kind::TextClip, text.layers()[0].clips[0].id}).size(), 1u);  // A non c'è nel gruppo
}

TEST(TextFades, FactorRisesAndFallsAndShrinksWhenTooLong)
{
    TextClip clip;
    clip.position = 10.0;
    clip.length = 4.0;
    clip.fadeIn = 2.0;
    clip.fadeOut = 1.0;
    EXPECT_NEAR(clip.fadeFactor(10.0), 0.0, 1e-9);
    EXPECT_NEAR(clip.fadeFactor(11.0), 0.5, 1e-9);
    EXPECT_NEAR(clip.fadeFactor(12.5), 1.0, 1e-9);
    EXPECT_NEAR(clip.fadeFactor(13.5), 0.5, 1e-9);
    clip.fadeIn = 3.0;
    clip.fadeOut = 3.0;  // insieme più lunghe della scritta: si accorciano in proporzione
    EXPECT_NEAR(clip.fadeFactor(12.0), 1.0, 1e-9);
    EXPECT_NEAR(clip.fadeFactor(11.0), 0.5, 1e-9);
}

TEST(TextFades, SetFadesClampsAndSplitDropsTheInnerOnes)
{
    TextTracks text;
    const int layer = text.addLayer();
    TextClip clip;
    clip.length = 4.0;
    clip.text = "A";
    const int id = text.add(layer, clip);
    ASSERT_NE(id, 0);
    EXPECT_TRUE(text.setFades(id, 1.0, 9.0));
    EXPECT_NEAR(text.find(id)->fadeIn, 1.0, 1e-9);
    EXPECT_NEAR(text.find(id)->fadeOut, 4.0, 1e-9);  // al massimo la durata
    EXPECT_FALSE(text.setFades(id, 1.0, 4.0));       // nessun cambiamento
    const int second = text.split(id, 2.0);
    ASSERT_NE(second, 0);
    EXPECT_NEAR(text.find(id)->fadeIn, 1.0, 1e-9);
    EXPECT_NEAR(text.find(id)->fadeOut, 0.0, 1e-9);
    EXPECT_NEAR(text.find(second)->fadeIn, 0.0, 1e-9);
    EXPECT_NEAR(text.find(second)->fadeOut, 4.0, 1e-9);
}

TEST(TextFades, FadesAndNamedStylesSurviveSaveAndReopen)
{
    Project project;
    const int layer = project.textTracks().addLayer();
    TextClip clip;
    clip.length = 3.0;
    clip.text = "Ciao";
    const int id = project.textTracks().add(layer, clip);
    ASSERT_NE(id, 0);
    project.textTracks().setFades(id, 0.5, 1.25);
    NamedTextStyle named;
    named.name = "Titolo";
    named.style.size = 0.12;
    named.style.bold = true;
    project.textStyles().push_back(named);
    NamedTextStyle twin = named;  // stesso nome: viene scartato
    project.textStyles().push_back(twin);
    const std::string json = projectToJson("C:/prova/p.evproj", project, ProjectView{});
    const LoadedProject loaded = projectFromJson("C:/prova/p.evproj", json);
    const TextClip* back = loaded.project.textTracks().find(loaded.project.textTracks().layers()[0].clips[0].id);
    ASSERT_NE(back, nullptr);
    EXPECT_NEAR(back->fadeIn, 0.5, 1e-6);
    EXPECT_NEAR(back->fadeOut, 1.25, 1e-6);
    ASSERT_EQ(loaded.project.textStyles().size(), 1u);
    EXPECT_EQ(loaded.project.textStyles()[0].name, "Titolo");
    EXPECT_TRUE(loaded.project.textStyles()[0].style.bold);
    EXPECT_NEAR(loaded.project.textStyles()[0].style.size, 0.12, 1e-9);
    const auto viaText = textStylesFromText(textStylesToText(loaded.project.textStyles()));
    ASSERT_EQ(viaText.size(), 1u);
    EXPECT_EQ(viaText[0], named);
    EXPECT_TRUE(textStylesFromText("non è json").empty());
}
