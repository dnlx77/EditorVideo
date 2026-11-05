#include "EditorCore/Animation.h"
#include "EditorCore/Json.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"

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
