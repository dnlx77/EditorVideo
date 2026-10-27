#include "EditorCore/Project.h"

#include <gtest/gtest.h>

using namespace editor::core;

namespace {

// Un file di 30 secondi e i pezzi dati (inizio, fine nel file), tutti attaccati uno dopo l'altro.
struct Fixture
{
    Project project;
    int clip = 0;
    std::vector<int> fragments;

    explicit Fixture(std::initializer_list<std::pair<double, double>> pieces, double duration = 30.0)
    {
        clip = project.add("C:/prova/video.mp4");
        project.setAnalysis(clip, {0.0}, duration);
        for (const auto& [start, end] : pieces)
            fragments.push_back(project.sequence().insert(-1, clip, start, end));
    }
};

}  // namespace

TEST(TransitionModel, CenteredOnTheJunctionWithinTheMargins)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}});
    EXPECT_TRUE(f.project.isJunction(f.fragments[0]));
    EXPECT_FALSE(f.project.isJunction(f.fragments[1]));  // l'ultimo pezzo non ha niente dopo
    const TransitionLimits limits = f.project.transitionLimits(f.fragments[0]);
    EXPECT_NEAR(limits.before, 4.0, 1e-9);  // al massimo quanto dura il pezzo prima
    EXPECT_NEAR(limits.after, 4.0, 1e-9);
    const int id = f.project.addTransition(f.fragments[0], "dissolvenza", 2.0);
    ASSERT_NE(id, 0);
    const auto placed = f.project.transitionPlacement(id);
    ASSERT_TRUE(placed.has_value());
    EXPECT_NEAR(placed->junction, 4.0, 1e-9);
    EXPECT_NEAR(placed->before, 1.0, 1e-9);
    EXPECT_NEAR(placed->after, 1.0, 1e-9);
    EXPECT_NEAR(placed->start(), 3.0, 1e-9);
    EXPECT_NEAR(placed->end(), 5.0, 1e-9);
    EXPECT_EQ(f.project.activeTransitions().size(), 1u);
}

TEST(TransitionModel, RefusesWhatDoesNotFit)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}});
    EXPECT_EQ(f.project.addTransition(f.fragments[0], "dissolvenza", 0.05), 0);   // sotto il minimo
    EXPECT_EQ(f.project.addTransition(f.fragments[0], "dissolvenza", 8.5), 0);    // più dei margini
    EXPECT_EQ(f.project.addTransition(f.fragments[0], "non-esiste", 1.0), 0);     // tipo sconosciuto
    EXPECT_EQ(f.project.addTransition(f.fragments[1], "dissolvenza", 1.0), 0);    // nessuna giunzione
    EXPECT_EQ(f.project.addTransition(999, "dissolvenza", 1.0), 0);               // pezzo inesistente
    const int id = f.project.addTransition(f.fragments[0], "dissolvenza", 8.0);   // tutto il margine
    ASSERT_NE(id, 0);
    EXPECT_EQ(f.project.addTransition(f.fragments[0], "scorrimento", 1.0), 0);    // già una lì
    EXPECT_NEAR(f.project.transitionPlacement(id)->before, 4.0, 1e-9);
    EXPECT_NEAR(f.project.transitionPlacement(id)->after, 4.0, 1e-9);
}

TEST(TransitionModel, ShiftsToTheSideThatHasMargin)
{
    // Il secondo pezzo inizia quasi all'inizio del file: poco margine prima della giunzione, tutto dopo.
    Fixture head({{10.0, 14.0}, {0.2, 4.2}});
    const int a = head.project.addTransition(head.fragments[0], "dissolvenza", 2.0);
    ASSERT_NE(a, 0);
    EXPECT_NEAR(head.project.transitionPlacement(a)->before, 0.2, 1e-9);
    EXPECT_NEAR(head.project.transitionPlacement(a)->after, 1.8, 1e-9);
    EXPECT_NEAR(head.project.transitionLimits(head.fragments[0]).max(), 4.2, 1e-9);
    // Il primo pezzo finisce proprio alla fine del file: niente dopo, tutto prima.
    Fixture tail({{26.0, 30.0}, {20.0, 24.0}});
    const int b = tail.project.addTransition(tail.fragments[0], "dissolvenza", 2.0);
    ASSERT_NE(b, 0);
    EXPECT_NEAR(tail.project.transitionPlacement(b)->before, 2.0, 1e-9);
    EXPECT_NEAR(tail.project.transitionPlacement(b)->after, 0.0, 1e-9);
    // Nessun margine da nessuna parte: non si può.
    Fixture none({{26.0, 30.0}, {0.0, 4.0}});
    EXPECT_EQ(none.project.addTransition(none.fragments[0], "dissolvenza", 1.0), 0);
    // Spazio vuoto fra i pezzi: non è una giunzione.
    Fixture gap({{10.0, 14.0}, {20.0, 24.0}});
    ASSERT_TRUE(gap.project.sequence().moveTo(gap.fragments[1], 6.0));
    EXPECT_FALSE(gap.project.isJunction(gap.fragments[0]));
    EXPECT_EQ(gap.project.addTransition(gap.fragments[0], "dissolvenza", 1.0), 0);
}

TEST(TransitionModel, AFileThatIsNotAnalyzedHasNoJunctions)
{
    Project project;
    const int clip = project.add("C:/prova/video.mp4");  // durata sconosciuta
    const int a = project.sequence().insert(-1, clip, 1.0, 5.0);
    project.sequence().insert(-1, clip, 8.0, 12.0);
    EXPECT_FALSE(project.isJunction(a));
    EXPECT_EQ(project.addTransition(a, "dissolvenza", 1.0), 0);
}

TEST(TransitionModel, NeighbouringTransitionsShareTheMiddlePiece)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}, {5.0, 9.0}});
    const int first = f.project.addTransition(f.fragments[0], "dissolvenza", 8.0);  // 4 prima e 4 dopo: tutto il secondo pezzo
    ASSERT_NE(first, 0);
    const TransitionLimits second = f.project.transitionLimits(f.fragments[1]);
    EXPECT_NEAR(second.before, 0.0, 1e-9);  // il secondo pezzo è già tutto della prima transizione
    EXPECT_NEAR(second.after, 4.0, 1e-9);
    EXPECT_EQ(f.project.addTransition(f.fragments[1], "dissolvenza", 5.0), 0);
    const int other = f.project.addTransition(f.fragments[1], "scorrimento", 4.0);
    ASSERT_NE(other, 0);
    EXPECT_NEAR(f.project.transitionPlacement(other)->before, 0.0, 1e-9);
    EXPECT_NEAR(f.project.transitionPlacement(other)->after, 4.0, 1e-9);
    const auto all = f.project.activeTransitions();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_LT(all[0].junction, all[1].junction);
    EXPECT_LE(all[0].end(), all[1].start() + 1e-9);  // non si sovrappongono
}

TEST(TransitionModel, ChangeDurationAndSettings)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}});
    const int id = f.project.addTransition(f.fragments[0], "dissolvenza", 1.0);
    ASSERT_NE(id, 0);
    EXPECT_TRUE(f.project.setTransitionDuration(id, 3.0));
    EXPECT_NEAR(f.project.transitions().find(id)->duration, 3.0, 1e-9);
    EXPECT_FALSE(f.project.setTransitionDuration(id, 8.5));
    EXPECT_FALSE(f.project.setTransitionDuration(id, 0.0));
    EXPECT_FALSE(f.project.setTransitionDuration(999, 1.0));
    EXPECT_NEAR(f.project.transitions().find(id)->duration, 3.0, 1e-9);  // nessuna modifica
    TransitionParams up;
    up.direction = TransitionDirection::Up;
    EXPECT_TRUE(f.project.setTransitionSettings(id, "scorrimento", up, false));
    const TransitionClip* clip = f.project.transitions().find(id);
    EXPECT_EQ(clip->type, "scorrimento");
    EXPECT_EQ(clip->params.direction, TransitionDirection::Up);
    EXPECT_FALSE(clip->crossfadeAudio);
    EXPECT_FALSE(f.project.setTransitionSettings(id, "non-esiste", up, true));
    EXPECT_EQ(f.project.transitions().find(id)->type, "scorrimento");
    EXPECT_TRUE(f.project.removeTransition(id));
    EXPECT_FALSE(f.project.removeTransition(id));
    EXPECT_TRUE(f.project.transitions().empty());
}

TEST(TransitionModel, PruneRemovesBrokenJunctionsAndShrinksLongOnes)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}, {5.0, 9.0}});
    const int first = f.project.addTransition(f.fragments[0], "dissolvenza", 8.0);
    ASSERT_NE(first, 0);
    ASSERT_NE(f.project.addTransition(f.fragments[1], "scorrimento", 4.0), 0);
    EXPECT_EQ(f.project.pruneTransitions(), 0);  // tutto a posto
    // Il secondo pezzo si accorcia di 2 secondi senza muovere i vicini: la prima transizione non ci sta più intera.
    ASSERT_TRUE(f.project.sequence().trim(f.fragments[1], Sequence::Edge::End, 22.0, 30.0, 0.1, true));
    EXPECT_FALSE(f.project.transitionPlacement(first).has_value());  // fuori dai limiti: non vale finché non si sistema
    EXPECT_GT(f.project.pruneTransitions(), 0);
    ASSERT_NE(f.project.transitions().find(first), nullptr);
    EXPECT_NEAR(f.project.transitions().find(first)->duration, 6.0, 1e-9);  // 4 prima + 2 dopo
    EXPECT_TRUE(f.project.transitionPlacement(first).has_value());
    // Un pezzo tolto porta via le transizioni sulle sue giunzioni.
    ASSERT_TRUE(f.project.sequence().remove(f.fragments[1]));
    EXPECT_FALSE(f.project.transitions().empty());
    EXPECT_GT(f.project.pruneTransitions(), 0);
    EXPECT_TRUE(f.project.transitions().empty());
}

TEST(TransitionModel, SplittingThePieceBeforeKeepsTheTransitionOnTheJunction)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}});
    const int id = f.project.addTransition(f.fragments[0], "dissolvenza", 2.0);
    ASSERT_NE(id, 0);
    const auto created = f.project.splitGroup({ObjectRef::Kind::Piece, f.fragments[0]}, 2.0);
    ASSERT_FALSE(created.empty());
    const int secondPart = created.front().id;
    const TransitionClip* clip = f.project.transitions().find(id);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->beforeFragment, secondPart);
    EXPECT_EQ(clip->afterFragment, f.fragments[1]);
    EXPECT_EQ(f.project.pruneTransitions(), 0);
    ASSERT_TRUE(f.project.transitionPlacement(id).has_value());
    EXPECT_NEAR(f.project.transitionPlacement(id)->junction, 4.0, 1e-9);
}

TEST(TransitionModel, UndoAndRedoRestoreTheTransitions)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}});
    const Project before = f.project;
    const int id = f.project.addTransition(f.fragments[0], "dissolvenza", 2.0);
    ASSERT_NE(id, 0);
    const Project after = f.project;
    f.project.restoreEdits(before);
    EXPECT_TRUE(f.project.transitions().empty());
    f.project.restoreEdits(after);
    ASSERT_NE(f.project.transitions().find(id), nullptr);
    EXPECT_TRUE(f.project.transitionPlacement(id).has_value());
    // Una transizione nuova dopo l'annullamento non riprende l'id di una tolta.
    f.project.restoreEdits(before);
    const int again = f.project.addTransition(f.fragments[0], "dissolvenza", 1.0);
    EXPECT_NE(again, id);
}

TEST(TransitionModel, UnknownTypesAreKeptButNotUsed)
{
    Fixture f({{10.0, 14.0}, {20.0, 24.0}});
    TransitionClip clip;
    clip.type = "tipo-del-futuro";
    clip.beforeFragment = f.fragments[0];
    clip.afterFragment = f.fragments[1];
    clip.duration = 1.0;
    const int id = f.project.mutableTransitions().add(clip);
    EXPECT_FALSE(f.project.transitionPlacement(id).has_value());
    EXPECT_TRUE(f.project.activeTransitions().empty());
    EXPECT_EQ(f.project.pruneTransitions(), 0);  // non si perde quello che questa versione non conosce
    EXPECT_NE(f.project.transitions().find(id), nullptr);
}

TEST(TransitionModel, UnanalyzedFilesDoNotLoseTheirTransitionsOnPrune)
{
    Project project;
    const int clip = project.add("C:/prova/video.mp4");  // appena aperto: non ancora analizzato
    const int a = project.sequence().insert(-1, clip, 10.0, 14.0);
    const int b = project.sequence().insert(-1, clip, 20.0, 24.0);
    TransitionClip t;
    t.type = "dissolvenza";
    t.beforeFragment = a;
    t.afterFragment = b;
    t.duration = 2.0;
    const int id = project.mutableTransitions().add(t);
    EXPECT_EQ(project.pruneTransitions(), 0);
    EXPECT_NE(project.transitions().find(id), nullptr);
    project.setAnalysis(clip, {0.0}, 30.0);
    EXPECT_TRUE(project.transitionPlacement(id).has_value());
}
