#include "EditorCore/Animation.h"
#include "EditorCore/Json.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/TextTracks.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace editor::core;

namespace {

AnimKey key(double time, double value, Interp interp = Interp::Linear)
{
    AnimKey k;
    k.time = time;
    k.value = value;
    k.interp = interp;
    return k;
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

TEST(Animation, NoKeysUsesTheFixedValueOneKeyIsConstant)
{
    EXPECT_DOUBLE_EQ(evaluateTrack({}, 3.0, 1.5), 1.5);
    EXPECT_DOUBLE_EQ(evaluateTrack({key(2.0, 0.7)}, -5.0, 1.5), 0.7);
    EXPECT_DOUBLE_EQ(evaluateTrack({key(2.0, 0.7)}, 99.0, 1.5), 0.7);
}

TEST(Animation, LinearInterpolatesAndClampsOutsideTheKeys)
{
    const std::vector<AnimKey> keys{key(1.0, 1.0), key(3.0, 2.0), key(4.0, 0.0)};
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 0.0, 9.0), 1.0);  // prima della prima
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 1.0, 9.0), 1.0);
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 2.0, 9.0), 1.5);
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 3.0, 9.0), 2.0);
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 3.5, 9.0), 1.0);
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 10.0, 9.0), 0.0);  // dopo l'ultima
}

TEST(Animation, HoldKeepsTheValueUntilTheNextKey)
{
    const std::vector<AnimKey> keys{key(0.0, 1.0, Interp::Hold), key(2.0, 5.0)};
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 1.99, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(evaluateTrack(keys, 2.0, 0.0), 5.0);
}

TEST(Animation, SmoothStartsAndEndsSlowlyAndIsSymmetric)
{
    const std::vector<AnimKey> keys{key(0.0, 0.0, Interp::Smooth), key(1.0, 1.0)};
    EXPECT_LT(evaluateTrack(keys, 0.1, 0.0), 0.1);  // più lenta di lineare all'inizio
    EXPECT_GT(evaluateTrack(keys, 0.9, 0.0), 0.9);  // e alla fine
    EXPECT_NEAR(evaluateTrack(keys, 0.5, 0.0), 0.5, 1e-6);
    EXPECT_NEAR(evaluateTrack(keys, 0.25, 0.0) + evaluateTrack(keys, 0.75, 0.0), 1.0, 1e-6);
    double previous = -1.0;
    for (int i = 0; i <= 100; ++i) {  // sempre crescente
        const double v = evaluateTrack(keys, i / 100.0, 0.0);
        EXPECT_GE(v, previous);
        previous = v;
    }
}

TEST(Animation, BezierWithLinearHandlesIsLinearAndOvershootIsAllowed)
{
    AnimKey a = key(0.0, 0.0, Interp::Bezier);
    a.c1x = 1.0 / 3.0;
    a.c1y = 1.0 / 3.0;
    a.c2x = 2.0 / 3.0;
    a.c2y = 2.0 / 3.0;
    const std::vector<AnimKey> linear{a, key(2.0, 10.0)};
    for (double t : {0.2, 0.7, 1.0, 1.6})
        EXPECT_NEAR(evaluateTrack(linear, t, 0.0), t * 5.0, 1e-6);

    a.c1y = 1.5;  // rimbalzo: esce oltre il valore finale
    a.c2y = 1.5;
    const std::vector<AnimKey> overshoot{a, key(2.0, 10.0)};
    double most = 0.0;
    for (int i = 0; i <= 200; ++i)
        most = std::max(most, evaluateTrack(overshoot, i / 100.0, 0.0));
    EXPECT_GT(most, 10.0);
    EXPECT_DOUBLE_EQ(evaluateTrack(overshoot, 2.0, 0.0), 10.0);
}

TEST(Animation, EvaluateTransformReplacesOnlyAnimatedParametersAndNormalizes)
{
    Transform base;
    base.posX = 40.0;
    base.rotation = 10.0;
    TransformAnimation anim;
    ASSERT_TRUE(setKey(anim, AnimParam::ScaleX, key(0.0, 1.0)));
    ASSERT_TRUE(setKey(anim, AnimParam::ScaleX, key(2.0, 2.0)));
    ASSERT_TRUE(setKey(anim, AnimParam::Opacity, key(0.0, 3.0)));  // fuori limite: si limita a 1
    const Transform t = evaluateTransform(base, anim, 1.0);
    EXPECT_DOUBLE_EQ(t.scaleX, 1.5);
    EXPECT_DOUBLE_EQ(t.scaleY, 1.0);  // non animata
    EXPECT_DOUBLE_EQ(t.posX, 40.0);
    EXPECT_DOUBLE_EQ(t.rotation, 10.0);
    EXPECT_DOUBLE_EQ(t.opacity, 1.0);
    // Senza animazione il risultato è la trasformazione fissa.
    EXPECT_EQ(evaluateTransform(base, TransformAnimation{}, 5.0), base);
}

TEST(Animation, EditingKeys)
{
    TransformAnimation anim;
    EXPECT_TRUE(anim.empty());
    EXPECT_TRUE(setKey(anim, AnimParam::PosX, key(2.0, 10.0)));
    EXPECT_TRUE(setKey(anim, AnimParam::PosX, key(1.0, 5.0)));  // si tiene in ordine
    EXPECT_EQ(anim.track(AnimParam::PosX).front().time, 1.0);
    EXPECT_FALSE(setKey(anim, AnimParam::PosX, key(1.0, 5.0)));  // uguale: niente cambia
    EXPECT_TRUE(setKey(anim, AnimParam::PosX, key(1.0, 6.0)));   // stesso tempo: sostituisce
    EXPECT_EQ(anim.track(AnimParam::PosX).size(), 2u);
    EXPECT_FALSE(setKey(anim, AnimParam::PosX, key(NAN, 1.0)));

    EXPECT_FALSE(moveKey(anim, AnimParam::PosX, 1.0, 2.0));  // lì c'è già una chiave
    EXPECT_FALSE(moveKey(anim, AnimParam::PosX, 7.0, 8.0));  // non c'è
    EXPECT_TRUE(moveKey(anim, AnimParam::PosX, 1.0, 3.0));   // passa dopo l'altra: si riordina
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::PosX).front().time, 2.0);
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::PosX).back().time, 3.0);

    EXPECT_TRUE(setKeyInterp(anim, AnimParam::PosX, 2.0, Interp::Hold));
    EXPECT_FALSE(setKeyInterp(anim, AnimParam::PosX, 2.0, Interp::Hold));
    EXPECT_TRUE(setKeyInterp(anim, AnimParam::PosX, 2.0, Interp::Bezier, 5.0, 0.2, -1.0, 0.9));  // x limitati a 0..1
    EXPECT_DOUBLE_EQ(findKey(anim, AnimParam::PosX, 2.0)->c1x, 1.0);
    EXPECT_DOUBLE_EQ(findKey(anim, AnimParam::PosX, 2.0)->c2x, 0.0);

    ASSERT_TRUE(setKey(anim, AnimParam::Opacity, key(0.5, 1.0)));
    const std::vector<double> times = keyTimes(anim);
    ASSERT_EQ(times.size(), 3u);
    EXPECT_DOUBLE_EQ(times[0], 0.5);

    EXPECT_TRUE(removeKey(anim, AnimParam::PosX, 3.0));
    EXPECT_FALSE(removeKey(anim, AnimParam::PosX, 3.0));
    EXPECT_TRUE(clearTrack(anim, AnimParam::PosX));
    EXPECT_FALSE(anim.animated(AnimParam::PosX));
    EXPECT_FALSE(anim.empty());  // resta l'opacità
}

TEST(AnimationModel, SetAnimationIsNormalizedAndReportsChanges)
{
    int id = 0;
    Project project = oneClip(id);
    TransformAnimation anim;
    anim.track(AnimParam::Rotation) = {key(4.0, 90.0), key(1.0, 0.0), key(4.0, 45.0)};  // fuori ordine, tempo doppio
    EXPECT_EQ(project.setAnimation({id, 9999}, anim), 1);
    const auto& keys = project.sequence().find(id)->animation.track(AnimParam::Rotation);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_DOUBLE_EQ(keys[0].time, 1.0);
    EXPECT_DOUBLE_EQ(keys[1].value, 45.0);  // vince l'ultima
    EXPECT_EQ(project.setAnimation({id}, anim), 0);
}

TEST(AnimationModel, SplitTrimAndRemoveKeepTheKeysAndTheLookDoesNotChange)
{
    int id = 0;
    Project project = oneClip(id);
    TransformAnimation anim;
    anim.track(AnimParam::ScaleX) = {key(0.0, 1.0, Interp::Smooth), key(10.0, 2.0)};
    project.setAnimation({id}, anim);
    const Transform before = evaluateTransform({}, project.sequence().find(id)->animation, 6.0);

    const int second = project.sequence().split(4.0);
    ASSERT_NE(second, 0);
    EXPECT_EQ(project.sequence().find(id)->animation, anim);
    EXPECT_EQ(project.sequence().find(second)->animation, anim);
    // Nel secondo pezzo l'immagine al secondo 6 del file ha lo stesso aspetto di prima della divisione.
    EXPECT_EQ(evaluateTransform({}, project.sequence().find(second)->animation, 6.0), before);

    ASSERT_TRUE(project.sequence().trim(second, Sequence::Edge::Start, 5.0, 30.0, 0.04, false));
    EXPECT_EQ(project.sequence().find(second)->animation, anim);
    const int file = project.sequence().find(id)->clipId;
    ASSERT_EQ(project.sequence().removeSourceRange(file, 1.0, 2.0), Sequence::RemoveResult::Removed);
    for (const Fragment& f : project.sequence().fragments())
        EXPECT_EQ(f.animation, anim);
}

TEST(AnimationModel, UndoRestoresTheAnimation)
{
    int id = 0;
    Project project = oneClip(id);
    const Project before = project;
    TransformAnimation anim;
    anim.track(AnimParam::Opacity) = {key(0.0, 0.0), key(1.0, 1.0)};
    project.setAnimation({id}, anim);
    project.restoreEdits(before);
    EXPECT_TRUE(project.sequence().find(id)->animation.empty());
}

TEST(AnimationFile, SavesAndReloadsPiecesAndLayerPieces)
{
    int id = 0;
    Project project = oneClip(id);
    ProjectView view;
    const std::string plain = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(parseJson(plain)["versione"].asInteger(), 1);
    EXPECT_EQ(plain.find("animazione"), std::string::npos);

    TransformAnimation anim;
    AnimKey b = key(1.25, 0.5, Interp::Bezier);
    b.c1x = 0.1;
    b.c1y = -0.5;
    b.c2x = 0.9;
    b.c2y = 1.4;
    anim.track(AnimParam::ScaleX) = {b, key(3.5, 1.5, Interp::Hold), key(7.0, 1.0, Interp::Smooth)};
    anim.track(AnimParam::PosY) = {key(-2.0, -100.5)};  // chiave fuori dal pezzo: resta
    project.setAnimation({id}, anim);

    const int file = project.sequence().find(id)->clipId;
    const int layer = project.textTracks().addLayer();
    LayerPiece piece;
    piece.clipId = file;
    piece.start = 1.0;
    piece.end = 4.0;
    piece.position = 2.0;
    piece.animation.track(AnimParam::Opacity) = {key(1.0, 0.0), key(2.0, 1.0)};
    ASSERT_NE(project.textTracks().addPiece(layer, piece), 0);

    const std::string json = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(parseJson(json)["versione"].asInteger(), 8);
    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    ASSERT_EQ(loaded.project.sequence().fragments().size(), 1u);
    const TransformAnimation& got = loaded.project.sequence().fragments()[0].animation;
    for (std::size_t p = 0; p < kAnimParamCount; ++p) {
        const AnimParam param = static_cast<AnimParam>(p);
        ASSERT_EQ(got.track(param).size(), anim.track(param).size());
        for (std::size_t i = 0; i < got.track(param).size(); ++i) {
            const AnimKey& a = got.track(param)[i];
            const AnimKey& e = anim.track(param)[i];
            EXPECT_NEAR(a.time, e.time, 1e-4);
            EXPECT_DOUBLE_EQ(a.value, e.value);
            EXPECT_EQ(a.interp, e.interp);
            if (e.interp == Interp::Bezier) {
                EXPECT_DOUBLE_EQ(a.c1x, e.c1x);
                EXPECT_DOUBLE_EQ(a.c1y, e.c1y);
                EXPECT_DOUBLE_EQ(a.c2x, e.c2x);
                EXPECT_DOUBLE_EQ(a.c2y, e.c2y);
            }
        }
    }
    ASSERT_EQ(loaded.project.textTracks().layers().size(), 1u);
    ASSERT_EQ(loaded.project.textTracks().layers()[0].pieces.size(), 1u);
    const AnimKey& op = loaded.project.textTracks().layers()[0].pieces[0].animation.track(AnimParam::Opacity)[1];
    EXPECT_DOUBLE_EQ(op.value, 1.0);
}

TEST(AnimationFile, BrokenKeysAreSkippedAndSorted)
{
    int id = 0;
    Project project = oneClip(id);
    TransformAnimation anim;
    anim.track(AnimParam::Rotation) = {key(1.0, 10.0), key(2.0, 20.0)};
    project.setAnimation({id}, anim);
    ProjectView view;
    std::string json = projectToJson("C:/prova/montaggio.evproj", project, view);
    const std::size_t at = json.find("\"v\": 10");
    ASSERT_NE(at, std::string::npos);
    json.replace(at, 7, "\"v\": \"x\"");  // un valore che non è un numero: la chiave si salta
    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    const auto& keys = loaded.project.sequence().fragments()[0].animation.track(AnimParam::Rotation);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_DOUBLE_EQ(keys[0].value, 20.0);
}

TEST(AnimationGroups, DiamondStartsTogglesAndEndsTheAnimation)
{
    Transform base;
    base.scaleX = 1.5;
    base.scaleY = 0.5;
    TransformAnimation anim;
    EXPECT_FALSE(groupState(anim, AnimGroup::Scale, 2.0, 0.0, 10.0).animated);

    // Primo clic: parte l'animazione con una chiave per parametro, col valore di adesso.
    ASSERT_TRUE(toggleGroupKey(anim, base, AnimGroup::Scale, 2.0));
    EXPECT_EQ(anim.track(AnimParam::ScaleX).size(), 1u);
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::ScaleX)[0].value, 1.5);
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::ScaleY)[0].value, 0.5);
    EXPECT_FALSE(anim.animated(AnimParam::PosX));  // gli altri gruppi non si toccano
    GroupState s = groupState(anim, AnimGroup::Scale, 2.0, 0.0, 10.0);
    EXPECT_TRUE(s.animated && s.keyAtCursor && !s.hasPrev && !s.hasNext);

    // Più avanti, senza chiave: un secondo clic ne mette una col valore interpolato (qui costante).
    s = groupState(anim, AnimGroup::Scale, 5.0, 0.0, 10.0);
    EXPECT_TRUE(s.animated && !s.keyAtCursor && s.hasPrev && !s.hasNext);
    EXPECT_DOUBLE_EQ(s.prevTime, 2.0);
    ASSERT_TRUE(toggleGroupKey(anim, base, AnimGroup::Scale, 5.0));
    EXPECT_EQ(anim.track(AnimParam::ScaleX).size(), 2u);

    // Una chiave quasi sul cursore (un fotogramma di differenza nel tempo) conta come sul cursore.
    s = groupState(anim, AnimGroup::Scale, 5.0009, 0.0, 10.0);
    EXPECT_TRUE(s.keyAtCursor);
    // Chiavi fuori dal pezzo non si raggiungono.
    s = groupState(anim, AnimGroup::Scale, 3.0, 0.0, 4.0);
    EXPECT_TRUE(s.hasPrev && !s.hasNext);

    // Clic su una chiave: la toglie; togliendo l'ultima l'animazione finisce e il valore resta quello della chiave.
    ASSERT_TRUE(toggleGroupKey(anim, base, AnimGroup::Scale, 5.0));
    EXPECT_EQ(anim.track(AnimParam::ScaleX).size(), 1u);
    anim.track(AnimParam::ScaleX)[0].value = 2.0;
    ASSERT_TRUE(toggleGroupKey(anim, base, AnimGroup::Scale, 2.0));
    EXPECT_FALSE(anim.animated(AnimParam::ScaleX));
    EXPECT_FALSE(anim.animated(AnimParam::ScaleY));
    EXPECT_DOUBLE_EQ(base.scaleX, 2.0);
    EXPECT_DOUBLE_EQ(base.scaleY, 0.5);
}

TEST(AnimationGroups, EditsOnAnimatedParametersBecomeKeysTheRestChangesTheBase)
{
    Transform base;
    TransformAnimation anim;
    anim.track(AnimParam::PosX) = {key(0.0, 0.0), key(4.0, 100.0)};
    anim.track(AnimParam::PosY) = {key(0.0, 0.0), key(4.0, 0.0)};
    const Transform shown = evaluateTransform(base, anim, 1.0);  // posX 25
    ASSERT_DOUBLE_EQ(shown.posX, 25.0);

    Transform edited = shown;
    edited.posX = 60.0;       // animato: diventa una chiave a 1 s
    edited.rotation = 15.0;   // non animato: cambia la trasformazione fissa
    edited.flipHorizontal = true;
    ASSERT_TRUE(applyEditWithKeys(anim, base, shown, edited, 1.0));
    ASSERT_EQ(anim.track(AnimParam::PosX).size(), 3u);
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::PosX)[1].time, 1.0);
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::PosX)[1].value, 60.0);
    EXPECT_EQ(anim.track(AnimParam::PosY).size(), 2u);  // posY non è cambiata: nessuna chiave nuova
    EXPECT_DOUBLE_EQ(base.posX, 0.0);                    // la base degli animati non si tocca
    EXPECT_DOUBLE_EQ(base.rotation, 15.0);
    EXPECT_TRUE(base.flipHorizontal);

    // Una seconda modifica quasi sullo stesso istante aggiorna la stessa chiave e ne tiene il tipo di passaggio.
    ASSERT_TRUE(setKeyInterp(anim, AnimParam::PosX, 1.0, Interp::Hold));
    const Transform shown2 = evaluateTransform(base, anim, 1.0005);
    Transform edited2 = shown2;
    edited2.posX = 70.0;
    ASSERT_TRUE(applyEditWithKeys(anim, base, shown2, edited2, 1.0005));
    ASSERT_EQ(anim.track(AnimParam::PosX).size(), 3u);
    EXPECT_DOUBLE_EQ(anim.track(AnimParam::PosX)[1].value, 70.0);
    EXPECT_EQ(anim.track(AnimParam::PosX)[1].interp, Interp::Hold);

    // Nessuna modifica: niente cambia.
    EXPECT_FALSE(applyEditWithKeys(anim, base, shown2, shown2, 1.0));
}

TEST(AnimationGroups, ResetClearsTheGroupOnly)
{
    Transform base;
    base.posX = 12.0;
    base.rotation = 30.0;
    TransformAnimation anim;
    anim.track(AnimParam::PosX) = {key(0.0, 0.0), key(1.0, 5.0)};
    anim.track(AnimParam::Rotation) = {key(0.0, 0.0), key(1.0, 90.0)};
    resetGroup(anim, base, AnimGroup::Position);
    EXPECT_FALSE(anim.animated(AnimParam::PosX));
    EXPECT_DOUBLE_EQ(base.posX, 0.0);
    EXPECT_TRUE(anim.animated(AnimParam::Rotation));
    EXPECT_DOUBLE_EQ(base.rotation, 30.0);
}

TEST(AnimationRow, DescribesWhereTheAnimationGoes)
{
    TransformAnimation a;
    EXPECT_TRUE(describeAnimation(Transform{}, a).empty());
    setKey(a, AnimParam::ScaleX, key(0.0, 1.0));
    setKey(a, AnimParam::ScaleY, key(0.0, 1.0));
    setKey(a, AnimParam::ScaleX, key(4.0, 1.3));
    setKey(a, AnimParam::ScaleY, key(4.0, 1.3));
    setKey(a, AnimParam::Opacity, key(0.0, 0.0));
    setKey(a, AnimParam::Opacity, key(1.0, 1.0));
    const std::vector<std::string> lines = describeAnimation(Transform{}, a);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "Scala 100%→130%");
    EXPECT_EQ(lines[1], "Opacità 0%→100%");
}

TEST(AnimationPresets, ZoomAnimatesTheScaleSmoothlyFromTheBaseValue)
{
    TransformAnimation a;
    Transform base;
    base.scaleX = base.scaleY = 1.2;
    ASSERT_TRUE(applyPreset(a, base, AnimPreset::ZoomIn, 2.0, 6.0, 1920));
    EXPECT_NEAR(evaluateTransform(base, a, 2.0).scaleX, 1.2, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 6.0).scaleX, 1.2 * 1.15, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 4.0).scaleX, 1.2 * 1.075, 1e-6);  // dolce: a metà è a metà
    EXPECT_NEAR(evaluateTransform(base, a, 4.0).scaleY, evaluateTransform(base, a, 4.0).scaleX, 1e-12);
    TransformAnimation out;
    Transform base2;
    ASSERT_TRUE(applyPreset(out, base2, AnimPreset::ZoomOut, 0.0, 3.0, 1920));
    EXPECT_NEAR(evaluateTransform(base2, out, 0.0).scaleX, 1.15, 1e-9);
    EXPECT_NEAR(evaluateTransform(base2, out, 3.0).scaleX, 1.0, 1e-9);
    EXPECT_FALSE(applyPreset(out, base2, AnimPreset::ZoomIn, 1.0, 1.05, 1920));  // troppo corto
}

TEST(AnimationPresets, PanZoomsTheImageAndSlidesItAcrossWithoutShowingTheEdges)
{
    TransformAnimation a;
    Transform base;
    ASSERT_TRUE(applyPreset(a, base, AnimPreset::PanRight, 0.0, 4.0, 1920));
    EXPECT_NEAR(base.scaleX, 1.15, 1e-9);  // il pezzo è ingrandito, non animato
    EXPECT_FALSE(a.animated(AnimParam::ScaleX));
    const double reach = 1920 * 0.15 / 2 * 0.9;
    EXPECT_NEAR(evaluateTransform(base, a, 0.0).posX, reach, 1e-6);
    EXPECT_NEAR(evaluateTransform(base, a, 4.0).posX, -reach, 1e-6);
    // Mai oltre il margine che l'ingrandimento nasconde: (1920 * 0.15 / 2 = 144 pixel per lato).
    for (double t = 0.0; t <= 4.0; t += 0.25)
        EXPECT_LE(std::abs(evaluateTransform(base, a, t).posX), 1920 * 0.15 / 2);
    TransformAnimation left;
    Transform base2;
    ASSERT_TRUE(applyPreset(left, base2, AnimPreset::PanLeft, 0.0, 4.0, 1920));
    EXPECT_NEAR(evaluateTransform(base2, left, 0.0).posX, -reach, 1e-6);
}

TEST(AnimationPresets, FadesCanBeCombinedAndKeepTheBaseOpacity)
{
    TransformAnimation a;
    Transform base;
    base.opacity = 0.8;
    ASSERT_TRUE(applyPreset(a, base, AnimPreset::FadeIn, 10.0, 20.0, 1920));
    ASSERT_TRUE(applyPreset(a, base, AnimPreset::FadeOut, 10.0, 20.0, 1920));
    EXPECT_NEAR(evaluateTransform(base, a, 10.0).opacity, 0.0, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 10.5).opacity, 0.4, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 15.0).opacity, 0.8, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 19.5).opacity, 0.4, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 20.0).opacity, 0.0, 1e-9);
    EXPECT_EQ(a.track(AnimParam::Opacity).size(), 4u);
    // Su un pezzo corto la dissolvenza è al massimo metà del pezzo.
    TransformAnimation shortOne;
    Transform base2;
    ASSERT_TRUE(applyPreset(shortOne, base2, AnimPreset::FadeIn, 0.0, 1.0, 1920));
    EXPECT_NEAR(shortOne.track(AnimParam::Opacity).back().time, 0.5, 1e-9);
    for (int i = 0; i < kAnimPresetCount; ++i)
        EXPECT_NE(std::string(animPresetName(static_cast<AnimPreset>(i))), "");
}

TEST(AnimationPresets, EntranceAndExitSlideAndFadeAtTheEdgesAndCanBeCombined)
{
    TransformAnimation a;
    Transform base;
    base.posX = 10.0;
    base.opacity = 0.9;
    ASSERT_TRUE(applyPreset(a, base, AnimPreset::SlideInLeft, 0.0, 5.0, 1800, 900));
    ASSERT_TRUE(applyPreset(a, base, AnimPreset::SlideOutRight, 0.0, 5.0, 1800, 900));
    EXPECT_NEAR(evaluateTransform(base, a, 0.0).posX, 10.0 - 600.0, 1e-9);  // fuori a sinistra
    EXPECT_NEAR(evaluateTransform(base, a, 0.0).opacity, 0.0, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 0.8).posX, 10.0, 1e-9);           // entrata finita: tratto 0,8 s
    EXPECT_NEAR(evaluateTransform(base, a, 0.8).opacity, 0.9, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 2.5).posX, 10.0, 1e-9);           // in mezzo: ferma
    EXPECT_NEAR(evaluateTransform(base, a, 4.2).posX, 10.0, 1e-9);
    EXPECT_NEAR(evaluateTransform(base, a, 5.0).posX, 10.0 + 600.0, 1e-9);  // esce a destra
    EXPECT_NEAR(evaluateTransform(base, a, 5.0).opacity, 0.0, 1e-9);
    // Dal basso: la Y parte un terzo dell'altezza più giù; pop: scala al 60%.
    TransformAnimation b;
    Transform baseB;
    ASSERT_TRUE(applyPreset(b, baseB, AnimPreset::SlideInBottom, 0.0, 3.0, 1800, 900));
    EXPECT_NEAR(evaluateTransform(baseB, b, 0.0).posY, 300.0, 1e-9);
    TransformAnimation c;
    Transform baseC;
    baseC.scaleX = baseC.scaleY = 2.0;
    ASSERT_TRUE(applyPreset(c, baseC, AnimPreset::PopIn, 0.0, 3.0, 1800, 900));
    EXPECT_NEAR(evaluateTransform(baseC, c, 0.0).scaleX, 1.2, 1e-9);
    EXPECT_NEAR(evaluateTransform(baseC, c, 3.0).scaleX, 2.0, 1e-9);
    // Su un pezzo corto il tratto è al massimo metà del pezzo.
    TransformAnimation d;
    Transform baseD;
    ASSERT_TRUE(applyPreset(d, baseD, AnimPreset::PopOut, 0.0, 1.0, 1800, 900));
    EXPECT_NEAR(d.track(AnimParam::Opacity).front().time, 0.5, 1e-9);
}

TEST(AnimationRow, GroupKeysAreEditedIndependentlyOfTheOtherGroupsAtTheSameInstant)
{
    TransformAnimation a;
    for (const AnimParam p : {AnimParam::ScaleX, AnimParam::ScaleY, AnimParam::Opacity}) {
        setKey(a, p, key(1.0, 1.0));
        setKey(a, p, key(3.0, 2.0));
    }
    setKey(a, AnimParam::PosX, key(1.0, 0.0));
    EXPECT_EQ(groupKeyTimes(a, AnimGroup::Scale), (std::vector<double>{1.0, 3.0}));
    EXPECT_EQ(groupKeyTimes(a, AnimGroup::Position), (std::vector<double>{1.0}));
    EXPECT_TRUE(groupKeyTimes(a, AnimGroup::Rotation).empty());

    EXPECT_FALSE(moveGroupKeys(a, AnimGroup::Scale, 1.0, 3.0));  // lì ha già una chiave
    EXPECT_TRUE(moveGroupKeys(a, AnimGroup::Scale, 1.0, 2.0));
    EXPECT_NE(findKey(a, AnimParam::ScaleY, 2.0), nullptr);
    EXPECT_NE(findKey(a, AnimParam::Opacity, 1.0), nullptr);  // l'opacità non si è mossa
    EXPECT_EQ(findKey(a, AnimParam::Opacity, 2.0), nullptr);
    EXPECT_FALSE(moveGroupKeys(a, AnimGroup::Rotation, 1.0, 2.0));

    EXPECT_TRUE(setGroupKeysInterp(a, AnimGroup::Opacity, 1.0, Interp::Hold));
    EXPECT_EQ(groupInterpAt(a, AnimGroup::Opacity, 1.0), Interp::Hold);
    EXPECT_EQ(groupInterpAt(a, AnimGroup::Scale, 2.0), Interp::Linear);  // la scala non è cambiata
    EXPECT_EQ(groupInterpAt(a, AnimGroup::Rotation, 1.0), Interp::Linear);

    Transform base;
    EXPECT_TRUE(removeGroupKeys(a, AnimGroup::Position, 1.0, &base));  // era l'unica: il valore resta nella base
    EXPECT_FALSE(a.animated(AnimParam::PosX));
    EXPECT_TRUE(a.animated(AnimParam::Opacity));
    EXPECT_TRUE(removeGroupKeys(a, AnimGroup::Opacity, 1.0));
    EXPECT_EQ(a.track(AnimParam::Opacity).size(), 1u);
    EXPECT_EQ(a.track(AnimParam::ScaleX).size(), 2u);
}

// ---- Scritte animate (S1): i tempi delle chiavi sono secondi dall'inizio della scritta.

namespace {

TextClip titleAt(double position, double length)
{
    TextClip clip;
    clip.position = position;
    clip.length = length;
    clip.text = "Titolo";
    return clip;
}

}  // namespace

TEST(TextAnimation, SetTransformAndAnimationReportChangesAndNormalize)
{
    TextTracks tracks;
    const int layer = tracks.addLayer();
    const int id = tracks.add(layer, titleAt(2.0, 6.0));
    ASSERT_NE(id, 0);
    Transform t;
    t.scaleX = t.scaleY = 1.5;
    EXPECT_TRUE(tracks.setTransform(id, t));
    EXPECT_FALSE(tracks.setTransform(id, t));
    EXPECT_FALSE(tracks.setTransform(9999, t));
    TransformAnimation anim;
    anim.track(AnimParam::Opacity) = {key(3.0, 1.0), key(0.0, 0.0), key(3.0, 0.5)};  // fuori ordine, tempo doppio
    EXPECT_TRUE(tracks.setAnimation(id, anim));
    const auto& keys = tracks.find(id)->animation.track(AnimParam::Opacity);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_DOUBLE_EQ(keys[0].time, 0.0);
    EXPECT_DOUBLE_EQ(keys[1].value, 0.5);
    EXPECT_FALSE(tracks.setAnimation(id, anim));
}

TEST(TextAnimation, MovingKeepsTheKeysTrimmingAndSplittingShiftThem)
{
    TextTracks tracks;
    const int layer = tracks.addLayer();
    const int id = tracks.add(layer, titleAt(2.0, 6.0));
    ASSERT_NE(id, 0);
    TransformAnimation anim;
    anim.track(AnimParam::Opacity) = {key(0.0, 0.0), key(4.0, 1.0)};
    tracks.setAnimation(id, anim);
    // Spostare la scritta non cambia i tempi (sono dall'inizio).
    ASSERT_TRUE(tracks.moveTo(id, layer, 10.0));
    EXPECT_EQ(tracks.find(id)->animation, anim);
    // Anticipare l'inizio di 1 s: la prima chiave ora sta 1 s dopo l'inizio, l'immagine non cambia.
    ASSERT_TRUE(tracks.setStart(id, 9.0));
    EXPECT_DOUBLE_EQ(tracks.find(id)->animation.track(AnimParam::Opacity)[0].time, 1.0);
    EXPECT_DOUBLE_EQ(tracks.find(id)->animation.track(AnimParam::Opacity)[1].time, 5.0);
    // Cambiare la fine non tocca le chiavi.
    ASSERT_TRUE(tracks.setEnd(id, 15.0));
    EXPECT_DOUBLE_EQ(tracks.find(id)->animation.track(AnimParam::Opacity)[1].time, 5.0);
    // Dividere a 12 (3 s dall'inizio): la seconda parte parte da 0, la prima resta com'è; l'aspetto non cambia.
    const double before = evaluateTransform({}, tracks.find(id)->animation, 4.5).opacity;
    const int second = tracks.split(id, 12.0);
    ASSERT_NE(second, 0);
    EXPECT_EQ(tracks.find(id)->animation.track(AnimParam::Opacity)[0].time, 1.0);
    EXPECT_NEAR(tracks.find(second)->animation.track(AnimParam::Opacity)[0].time, -2.0, 1e-9);
    EXPECT_NEAR(evaluateTransform({}, tracks.find(second)->animation, 4.5 - 3.0).opacity, before, 1e-9);
}

TEST(TextAnimation, SavesAndReloadsTheTransformAndTheKeysOfTexts)
{
    Project project;
    ProjectView view;
    const int layer = project.textTracks().addLayer();
    TextClip clip = titleAt(1.0, 5.0);
    clip.transform.scaleX = clip.transform.scaleY = 1.25;
    clip.transform.rotation = 15.0;
    clip.animation.track(AnimParam::PosX) = {key(0.0, -100.0, Interp::Smooth), key(2.5, 40.0)};
    ASSERT_NE(project.textTracks().add(layer, clip), 0);
    TextClip base = titleAt(0.0, 2.0);  // una scritta di V1 con la sola trasformazione
    base.transform.flipHorizontal = true;
    ASSERT_NE(project.textTracks().add(TextTracks::kBaseLayerId, base), 0);

    const std::string json = projectToJson("C:/prova/montaggio.evproj", project, view);
    EXPECT_EQ(parseJson(json)["versione"].asInteger(), 8);
    const LoadedProject loaded = projectFromJson("C:/prova/montaggio.evproj", json);
    ASSERT_EQ(loaded.project.textTracks().layers().size(), 1u);
    const TextClip& got = loaded.project.textTracks().layers()[0].clips[0];
    EXPECT_DOUBLE_EQ(got.transform.scaleX, 1.25);
    EXPECT_DOUBLE_EQ(got.transform.rotation, 15.0);
    ASSERT_EQ(got.animation.track(AnimParam::PosX).size(), 2u);
    EXPECT_NEAR(got.animation.track(AnimParam::PosX)[1].time, 2.5, 1e-4);
    EXPECT_EQ(got.animation.track(AnimParam::PosX)[0].interp, Interp::Smooth);
    ASSERT_EQ(loaded.project.textTracks().baseLayer().clips.size(), 1u);
    EXPECT_TRUE(loaded.project.textTracks().baseLayer().clips[0].transform.flipHorizontal);

    // Senza animazione il file resta nella versione 6 (così si apre con i programmi di prima) e un testo normale non scrive niente.
    Project plain;
    const int l2 = plain.textTracks().addLayer();
    TextClip fixed = titleAt(0.0, 2.0);
    fixed.transform.rotation = 10.0;
    ASSERT_NE(plain.textTracks().add(l2, fixed), 0);
    EXPECT_EQ(parseJson(projectToJson("C:/prova/montaggio.evproj", plain, view))["versione"].asInteger(), 6);
    Project untouched;
    ASSERT_NE(untouched.textTracks().add(untouched.textTracks().addLayer(), titleAt(0.0, 2.0)), 0);
    const std::string untouchedJson = projectToJson("C:/prova/montaggio.evproj", untouched, view);
    EXPECT_EQ(untouchedJson.find("trasformazione"), std::string::npos);
    EXPECT_EQ(untouchedJson.find("animazione"), std::string::npos);
}
