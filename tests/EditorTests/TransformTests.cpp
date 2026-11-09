#include "EditorCore/Project.h"
#include "EditorCore/Json.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/Transform.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace editor::core;

namespace {

void expectPoint(Point2 p, double x, double y, double tolerance = 1e-9)
{
    EXPECT_NEAR(p.x, x, tolerance);
    EXPECT_NEAR(p.y, y, tolerance);
}

Project oneClip(int& fragment)
{
    Project project;
    const int file = project.add("video.mp4");
    project.setAnalysis(file, {0.0}, 30.0);
    fragment = project.sequence().insert(-1, file, 0.0, 10.0);
    return project;
}

}  // namespace

TEST(Transform, DefaultIsIdentityAndFitsTheProjectFrame)
{
    EXPECT_TRUE(Transform{}.isIdentity());
    // File 1920x1080 in un progetto 1920x1080: stesso riquadro.
    const TransformGeometry same = computeGeometry({}, 1920, 1080, 1920, 1080);
    expectPoint(same.corners[0], 0, 0);
    expectPoint(same.corners[2], 1920, 1080);
    // File 4:3 (800x600) in 1920x1080: adattato in altezza, bande nere ai lati (scala 1.8).
    const TransformGeometry fit = computeGeometry({}, 800, 600, 1920, 1080);
    expectPoint(fit.corners[0], 240, 0);
    expectPoint(fit.corners[2], 1680, 1080);
}

TEST(Transform, ScaleAndPositionMoveTheCenter)
{
    Transform t;
    t.scaleX = 0.5;
    t.scaleY = 0.5;
    t.posX = 100;
    t.posY = -50;
    const TransformGeometry g = computeGeometry(t, 1920, 1080, 1920, 1080);
    // Metà grandezza, centro a (960 + 100, 540 - 50).
    expectPoint(g.corners[0], 1060 - 480, 490 - 270);
    expectPoint(g.corners[2], 1060 + 480, 490 + 270);
    // Scale diverse: larghezza dimezzata, altezza intera.
    Transform wide;
    wide.scaleX = 0.5;
    const TransformGeometry w = computeGeometry(wide, 1920, 1080, 1920, 1080);
    expectPoint(w.corners[0], 480, 0);
    expectPoint(w.corners[2], 1440, 1080);
}

TEST(Transform, RotationIsClockwiseAroundTheCenter)
{
    Transform t;
    t.rotation = 90.0;
    const TransformGeometry g = computeGeometry(t, 200, 100, 200, 100);
    // Il centro è (100, 50). L'angolo in alto a sinistra (0,0) a 90° orari va in alto a destra del nuovo riquadro.
    expectPoint(g.corners[0], 100 + 50, 50 - 100, 1e-9);
    expectPoint(g.corners[1], 100 + 50, 50 + 100, 1e-9);
    // Il centro del file resta al centro del progetto.
    expectPoint(g.toProject.apply(100, 50), 100, 50);
}

TEST(Transform, CropKeepsScaleAndPositionOfWhatRemains)
{
    Transform t;
    t.cropLeft = 0.25;
    t.cropBottom = 0.5;
    const TransformGeometry g = computeGeometry(t, 1000, 500, 1000, 500);
    EXPECT_DOUBLE_EQ(g.sourceLeft, 250);
    EXPECT_DOUBLE_EQ(g.sourceRight, 1000);
    EXPECT_DOUBLE_EQ(g.sourceTop, 0);
    EXPECT_DOUBLE_EQ(g.sourceBottom, 250);
    expectPoint(g.corners[0], 250, 0);
    expectPoint(g.corners[2], 1000, 250);
    // Il riquadro intero non cambia.
    expectPoint(g.fullCorners[0], 0, 0);
    expectPoint(g.fullCorners[2], 1000, 500);
}

TEST(Transform, FlipMirrorsTheImageAndKeepsCropOnTheScreenSide)
{
    Transform t;
    t.flipHorizontal = true;
    t.cropLeft = 0.25;  // il quarto sinistro dello SCHERMO: con lo specchio è il quarto destro del file
    const TransformGeometry g = computeGeometry(t, 1000, 500, 1000, 500);
    EXPECT_DOUBLE_EQ(g.sourceLeft, 0);
    EXPECT_DOUBLE_EQ(g.sourceRight, 750);
    // Il pixel sinistro del file va a destra del progetto.
    expectPoint(g.toProject.apply(0, 0), 1000, 0);
    expectPoint(g.toProject.apply(1000, 0), 0, 0);
    // La parte visibile sullo schermo va da x=250 a x=1000.
    expectPoint(g.toProject.apply(750, 0), 250, 0);
}

TEST(Transform, InverseGoesBack)
{
    Transform t;
    t.scaleX = 0.7;
    t.scaleY = 1.3;
    t.rotation = 33.0;
    t.posX = 12.5;
    t.posY = -40;
    const TransformGeometry g = computeGeometry(t, 1280, 720, 1920, 1080);
    const Affine inv = g.toProject.inverse();
    const Point2 p = g.toProject.apply(321.5, 87.25);
    expectPoint(inv.apply(p.x, p.y), 321.5, 87.25, 1e-9);
}

TEST(Transform, NormalizedLimitsTheValues)
{
    Transform t;
    t.scaleX = 0.0;
    t.scaleY = 1e9;
    t.posX = std::nan("");
    t.rotation = 99999;
    t.cropLeft = 0.9;
    t.cropRight = 0.9;  // la somma supera il limite: si riducono in proporzione
    t.cropTop = -1;
    t.opacity = 2.0;
    const Transform n = normalized(t);
    EXPECT_DOUBLE_EQ(n.scaleX, kMinScale);
    EXPECT_DOUBLE_EQ(n.scaleY, kMaxScale);
    EXPECT_DOUBLE_EQ(n.posX, 0.0);
    EXPECT_DOUBLE_EQ(n.rotation, kMaxRotation);
    EXPECT_NEAR(n.cropLeft + n.cropRight, kMaxCrop, 1e-12);
    EXPECT_DOUBLE_EQ(n.cropLeft, n.cropRight);
    EXPECT_DOUBLE_EQ(n.cropTop, 0.0);
    EXPECT_DOUBLE_EQ(n.opacity, 1.0);
    EXPECT_EQ(normalized(Transform{}), Transform{});
}

TEST(Transform, OpacityAloneDoesNotMoveThePicture)
{
    Transform t;
    t.opacity = 0.5;
    EXPECT_FALSE(t.isIdentity());
    EXPECT_FALSE(t.movesPicture());
    t.rotation = 1.0;
    EXPECT_TRUE(t.movesPicture());
}

TEST(TransformModel, SetTransformIsNormalizedAndReportsChanges)
{
    int id = 0;
    Project project = oneClip(id);
    Transform t;
    t.scaleX = 0.5;
    t.scaleY = 0.5;
    EXPECT_EQ(project.setTransform({id, 9999}, t), 1);  // l'id che non c'è si salta
    EXPECT_EQ(project.sequence().find(id)->transform, t);
    EXPECT_EQ(project.setTransform({id}, t), 0);  // niente è cambiato
    Transform wild;
    wild.scaleX = -3;
    EXPECT_EQ(project.setTransform({id}, wild), 1);
    EXPECT_DOUBLE_EQ(project.sequence().find(id)->transform.scaleX, kMinScale);
}

TEST(TransformModel, BothHalvesKeepTheTransformWhenSplit)
{
    int id = 0;
    Project project = oneClip(id);
    Transform t;
    t.rotation = 15;
    t.cropTop = 0.1;
    project.setTransform({id}, t);
    const int second = project.sequence().split(4.0);
    ASSERT_NE(second, 0);
    EXPECT_EQ(project.sequence().find(id)->transform, t);
    EXPECT_EQ(project.sequence().find(second)->transform, t);
}

TEST(TransformModel, TrimAndRemoveSourceRangeKeepTheTransform)
{
    int id = 0;
    Project project = oneClip(id);
    Transform t;
    t.posX = 40;
    project.setTransform({id}, t);
    ASSERT_TRUE(project.sequence().trim(id, Sequence::Edge::End, 8.0, 30.0, 0.04, false));
    EXPECT_EQ(project.sequence().find(id)->transform, t);
    // X in mezzo al pezzo: le due parti che restano hanno la trasformazione.
    const int file = project.sequence().find(id)->clipId;
    ASSERT_EQ(project.sequence().removeSourceRange(file, 3.0, 5.0), Sequence::RemoveResult::Removed);
    ASSERT_EQ(project.sequence().fragments().size(), 2u);
    for (const Fragment& f : project.sequence().fragments())
        EXPECT_EQ(f.transform, t);
}

TEST(TransformModel, UndoRestoresTheTransform)
{
    int id = 0;
    Project project = oneClip(id);
    const Project before = project;
    Transform t;
    t.scaleX = 2.0;
    project.setTransform({id}, t);
    project.restoreEdits(before);
    EXPECT_TRUE(project.sequence().find(id)->transform.isIdentity());
}

TEST(TransformFile, SavesAndReloadsAndOldFilesStayOldVersion)
{
    int id = 0;
    Project project = oneClip(id);
    ProjectView view;
    // Senza trasformazioni il file non ha la chiave.
    const std::string plain = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(parseJson(plain)["versione"].asInteger(), kProjectFormatVersion);
    EXPECT_EQ(plain.find("trasformazione"), std::string::npos);

    Transform t;
    t.scaleX = 0.75;
    t.scaleY = 0.5;
    t.posX = -123.5;
    t.posY = 7.25;
    t.rotation = -33.3;
    t.cropLeft = 0.125;
    t.cropBottom = 0.0625;
    t.flipVertical = true;
    t.opacity = 0.4;
    project.setTransform({id}, t);
    const std::string json = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(parseJson(json)["versione"].asInteger(), kProjectFormatVersion);

    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    ASSERT_EQ(loaded.project.sequence().fragments().size(), 1u);
    EXPECT_EQ(loaded.project.sequence().fragments()[0].transform, t);
}

TEST(TransformFile, BrokenValuesFallBackToBase)
{
    int id = 0;
    Project project = oneClip(id);
    Transform t;
    t.scaleX = 0.5;
    project.setTransform({id}, t);
    ProjectView view;
    std::string json = projectToJson("C:/prova/montaggio.evproj", project, view);
    const std::size_t at = json.find("\"scalaX\": 0.5");
    ASSERT_NE(at, std::string::npos);
    json.replace(at, std::string("\"scalaX\": 0.5").size(), "\"scalaX\": \"molto\", \"rotazione\": 99999, \"ritaglioAlto\": 5");
    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    const Transform& r = loaded.project.sequence().fragments()[0].transform;
    EXPECT_DOUBLE_EQ(r.scaleX, 1.0);  // un valore che non è un numero torna a quello di base
    EXPECT_DOUBLE_EQ(r.rotation, kMaxRotation);  // fuori dai limiti: si limita
    EXPECT_DOUBLE_EQ(r.cropTop, kMaxCrop);
}

TEST(Transform, DescribeListsOnlyWhatChanged)
{
    EXPECT_TRUE(describeTransform(Transform{}).empty());

    Transform t;
    t.scaleX = t.scaleY = 1.5;
    t.posX = 120.0;
    t.rotation = -12.5;
    t.cropLeft = 0.1;
    t.cropBottom = 0.25;
    t.flipHorizontal = true;
    t.opacity = 0.5;
    const std::vector<std::string> lines = describeTransform(t);
    ASSERT_EQ(lines.size(), 6u);
    EXPECT_EQ(lines[0], "Scala 150%");
    EXPECT_EQ(lines[1], "Posizione 120, 0 px");
    EXPECT_EQ(lines[2], "Rotazione -12,5°");
    EXPECT_EQ(lines[3], "Ritaglio sinistra 10% basso 25%");
    EXPECT_EQ(lines[4], "Specchiato in orizzontale");
    EXPECT_EQ(lines[5], "Opacità 50%");

    Transform stretched;
    stretched.scaleX = 2.0;
    EXPECT_EQ(describeTransform(stretched).at(0), "Scala 200% × 100%");
    Transform opacityOnly;
    opacityOnly.opacity = 0.4;
    EXPECT_EQ(describeTransform(opacityOnly), std::vector<std::string>{"Opacità 40%"});
}
