#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/TextTracks.h"

#include <gtest/gtest.h>

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
