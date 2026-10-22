#include "EditorCore/Sequence.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace editor::core;

namespace {

struct Piece
{
    int clip;
    double start;
    double end;
};

std::vector<Piece> pieces(const Sequence& s)
{
    std::vector<Piece> out;
    for (const Fragment& f : s.fragments())
        out.push_back({f.clipId, f.start, f.end});
    return out;
}

void expectPieces(const Sequence& s, std::initializer_list<Piece> expected)
{
    const auto actual = pieces(s);
    ASSERT_EQ(actual.size(), expected.size());
    std::size_t i = 0;
    for (const Piece& e : expected) {
        EXPECT_EQ(actual[i].clip, e.clip) << "pezzo " << i;
        EXPECT_NEAR(actual[i].start, e.start, 1e-9) << "pezzo " << i;
        EXPECT_NEAR(actual[i].end, e.end, 1e-9) << "pezzo " << i;
        ++i;
    }
}

TEST(Sequence, InsertKeepsTheTimesAndAcceptsAnyOrderAndRepeats)
{
    Sequence s;
    EXPECT_NE(s.insert(-1, 1, 4.7, 6.0), 0);
    EXPECT_NE(s.insert(-1, 2, 0.5, 1.5), 0);
    EXPECT_NE(s.insert(-1, 1, 0.0, 1.0), 0);   // lo stesso file di nuovo, prima nel tempo
    EXPECT_NE(s.insert(-1, 3, -0.5, 1.0), 0);  // prima dell'inizio del file: da 0
    expectPieces(s, {{1, 4.7, 6.0}, {2, 0.5, 1.5}, {1, 0.0, 1.0}, {3, 0.0, 1.0}});
    EXPECT_NEAR(s.totalLength(), 4.3, 1e-9);
}

TEST(Sequence, InsertAtPosition)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 2.0);
    s.insert(-1, 1, 4.0, 6.0);
    s.insert(1, 2, 3.0, 3.5);   // in mezzo
    s.insert(0, 2, 5.0, 6.0);   // in testa
    s.insert(99, 3, 1.0, 2.0);  // oltre la fine: in fondo
    expectPieces(s, {{2, 5.0, 6.0}, {1, 0.0, 2.0}, {2, 3.0, 3.5}, {1, 4.0, 6.0}, {3, 1.0, 2.0}});
}

TEST(Sequence, EmptyOrInvertedPieceIsRejected)
{
    Sequence s;
    EXPECT_EQ(s.insert(-1, 1, 3.0, 3.0), 0);
    EXPECT_EQ(s.insert(-1, 1, 3.0, 2.0), 0);
    EXPECT_EQ(s.insert(-1, 1, 3.0, 3.0005), 0);  // più corto di kTimeEpsilon
    EXPECT_TRUE(s.empty());
}

TEST(Sequence, PositionsInTheSequence)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 2.0);  // 0 .. 2
    s.insert(-1, 2, 1.0, 1.5);  // 2 .. 2.5
    s.insert(-1, 1, 4.0, 7.0);  // 2.5 .. 5.5
    EXPECT_NEAR(s.startOf(2), 2.5, 1e-9);
    EXPECT_EQ(s.indexAt(0.0), 0);
    EXPECT_EQ(s.indexAt(2.0), 1);
    EXPECT_EQ(s.indexAt(2.4), 1);
    EXPECT_EQ(s.indexAt(5.0), 2);
    EXPECT_EQ(s.indexAt(5.5), -1);  // oltre la fine
}

TEST(Sequence, MoveAndRemove)
{
    Sequence s;
    const int a = s.insert(-1, 1, 0.0, 1.0);
    const int b = s.insert(-1, 2, 0.0, 1.0);
    const int c = s.insert(-1, 3, 0.0, 1.0);
    s.move(c, 0);
    expectPieces(s, {{3, 0.0, 1.0}, {1, 0.0, 1.0}, {2, 0.0, 1.0}});
    s.move(c, 99);
    expectPieces(s, {{1, 0.0, 1.0}, {2, 0.0, 1.0}, {3, 0.0, 1.0}});
    EXPECT_TRUE(s.remove(b));
    EXPECT_FALSE(s.remove(b));
    EXPECT_EQ(s.indexOf(a), 0);
    EXPECT_EQ(s.indexOf(c), 1);
    EXPECT_EQ(s.find(b), nullptr);
}

TEST(Sequence, RemoveClipTakesAllItsPieces)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 1.0);
    s.insert(-1, 2, 0.0, 1.0);
    s.insert(-1, 1, 4.0, 5.0);
    s.removeClip(1);
    expectPieces(s, {{2, 0.0, 1.0}});
}

TEST(Sequence, SplitAtThePlayhead)
{
    Sequence s;
    s.insert(-1, 2, 0.0, 1.0);  // 0 .. 1 nella sequenza
    s.insert(-1, 1, 0.0, 8.0);  // 1 .. 9 nella sequenza
    // A 5,3 s della sequenza siamo a 4,3 s del file 1: si divide proprio lì.
    const int second = s.split(5.3);
    EXPECT_NE(second, 0);
    expectPieces(s, {{2, 0.0, 1.0}, {1, 0.0, 4.3}, {1, 4.3, 8.0}});
    EXPECT_EQ(s.indexOf(second), 2);
    EXPECT_NEAR(s.totalLength(), 9.0, 1e-9);  // niente perso né ripetuto
}

TEST(Sequence, SplitImpossibleOnTheEdgeOrOutside)
{
    Sequence s;
    s.insert(-1, 1, 2.0, 3.5);
    EXPECT_EQ(s.split(0.0), 0);          // sul bordo: non resterebbe niente prima
    EXPECT_EQ(s.split(0.0005), 0);       // a meno di kTimeEpsilon dal bordo
    EXPECT_EQ(s.split(10.0), 0);         // oltre la fine
    expectPieces(s, {{1, 2.0, 3.5}});
}

TEST(Sequence, RemoveSourceRangeFromPieces)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 10.0);
    s.insert(-1, 2, 0.0, 3.0);
    s.insert(-1, 1, 0.0, 10.0);  // lo stesso file due volte: il pezzo tolto sparisce da entrambi
    EXPECT_EQ(s.removeSourceRange(1, 3.0, 6.0), Sequence::RemoveResult::Removed);
    expectPieces(s, {{1, 0.0, 3.0}, {1, 6.0, 10.0}, {2, 0.0, 3.0}, {1, 0.0, 3.0}, {1, 6.0, 10.0}});
}

TEST(Sequence, RemoveSourceRangeReasons)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 4.0);
    EXPECT_EQ(s.removeSourceRange(1, 2.0, 2.0), Sequence::RemoveResult::Empty);
    EXPECT_EQ(s.removeSourceRange(1, 6.0, 8.0), Sequence::RemoveResult::OutsideKept);  // fuori dai pezzi
    EXPECT_EQ(s.removeSourceRange(2, 0.0, 1.0), Sequence::RemoveResult::OutsideKept);  // file non usato
    expectPieces(s, {{1, 0.0, 4.0}});
}

TEST(Sequence, RemoveSourceRangeCutsExactly)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 4.0);
    EXPECT_EQ(s.removeSourceRange(1, 2.5, 3.5), Sequence::RemoveResult::Removed);  // la parte dopo riparte da 3,5
    expectPieces(s, {{1, 0.0, 2.5}, {1, 3.5, 4.0}});
}

// ---------------------------------------------------------------------------------------------------------------
// Spazi vuoti

// Tre pezzi da 1, 2 e 1 secondi, attaccati: A 0 .. 1, B 1 .. 3, C 3 .. 4.
struct ThreePieces
{
    Sequence s;
    int a = 0;
    int b = 0;
    int c = 0;

    ThreePieces()
    {
        a = s.insert(-1, 2, 0.0, 1.0);
        b = s.insert(-1, 2, 2.0, 4.0);
        c = s.insert(-1, 2, 5.0, 6.0);
    }
};

TEST(SequenceGaps, MovingAPieceLeavesTheOthersWhereTheyAre)
{
    ThreePieces p;
    EXPECT_TRUE(p.s.moveTo(p.c, 6.0));  // C più avanti: fra B e C uno spazio di 3 s
    EXPECT_NEAR(p.s.startOf(0), 0.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(1), 1.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(2), 6.0, 1e-9);
    EXPECT_NEAR(p.s.totalLength(), 7.0, 1e-9);
    EXPECT_EQ(p.s.gapCount(), 1);
    EXPECT_EQ(p.s.indexAt(4.5), -1);    // nello spazio non c'è nessun pezzo
    EXPECT_EQ(p.s.gapIndexAt(4.5), 2);  // è lo spazio prima di C
    EXPECT_EQ(p.s.gapIndexAt(0.5), -1);
    EXPECT_EQ(p.s.indexAt(6.5), 2);

    EXPECT_TRUE(p.s.moveTo(p.a, 4.0));  // A nello spazio: B e C non si muovono, l'ordine cambia
    expectPieces(p.s, {{2, 2.0, 4.0}, {2, 0.0, 1.0}, {2, 5.0, 6.0}});
    EXPECT_NEAR(p.s.startOf(0), 1.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(1), 4.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(2), 6.0, 1e-9);
    EXPECT_EQ(p.s.gapCount(), 3);  // prima di B, fra B e A, fra A e C
}

TEST(SequenceGaps, PiecesNeverOverlap)
{
    ThreePieces p;
    EXPECT_FALSE(p.s.moveTo(p.a, 0.5));  // su B
    EXPECT_FALSE(p.s.moveTo(p.c, 2.5));  // su B
    EXPECT_FALSE(p.s.moveTo(p.b, -1.0));
    expectPieces(p.s, {{2, 0.0, 1.0}, {2, 2.0, 4.0}, {2, 5.0, 6.0}});  // niente è cambiato
    EXPECT_EQ(p.s.gapCount(), 0);
    EXPECT_TRUE(p.s.moveTo(p.c, 3.0));  // dove già era: va bene
}

TEST(SequenceGaps, NearestFreeStartStopsAgainstTheNeighbours)
{
    ThreePieces p;
    ASSERT_TRUE(p.s.moveTo(p.c, 6.0));  // A 0 .. 1, B 1 .. 3, spazio 3 .. 6, C 6 .. 7
    // B (2 s) fra A e C: ci sta da 1 a 4.
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 2.0), 2.0, 1e-9);
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 5.2), 4.0, 1e-9);  // si ferma contro C
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 0.0), 1.0, 1e-9);  // si ferma contro A
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 6.6), 7.0, 1e-9);  // trascinata oltre C: dopo C, più vicino
    // A (1 s) trascinata su B: va nello spazio libero più vicino.
    EXPECT_NEAR(p.s.nearestFreeStart(p.a, 2.5), 3.0, 1e-9);
    EXPECT_NEAR(p.s.nearestFreeStart(p.a, 1.2), 0.0, 1e-9);
    EXPECT_NEAR(p.s.nearestFreeStart(p.a, -3.0), 0.0, 1e-9);
    EXPECT_LT(p.s.nearestFreeStart(999, 1.0), 0.0);  // pezzo che non c'è
}

TEST(SequenceGaps, InsertingFillsTheGapFromItsStart)
{
    ThreePieces p;
    ASSERT_TRUE(p.s.moveTo(p.c, 6.0));  // spazio di 3 s prima di C
    // Un pezzo da 2 s nello spazio: parte dall'inizio dello spazio, C non si muove.
    p.s.insert(2, 2, 0.0, 2.0);
    EXPECT_NEAR(p.s.startOf(2), 3.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(3), 6.0, 1e-9);
    EXPECT_NEAR(p.s.fragments()[3].gap, 1.0, 1e-9);
    // Un altro da 2 s: nello spazio rimasto (1 s) non ci sta, C va avanti di quanto serve.
    p.s.insert(3, 2, 0.0, 2.0);
    EXPECT_NEAR(p.s.startOf(3), 5.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(4), 7.0, 1e-9);
    EXPECT_EQ(p.s.gapCount(), 0);
}

TEST(SequenceGaps, ClosingAndRemoving)
{
    ThreePieces p;
    ASSERT_TRUE(p.s.moveTo(p.c, 7.0));  // C 7 .. 8
    ASSERT_TRUE(p.s.moveTo(p.b, 3.0));  // A 0 .. 1, spazio 2, B 3 .. 5, spazio 2, C 7 .. 8
    // Togliendo B, chi viene dopo scorre indietro della durata di B; lo spazio prima di B resta.
    EXPECT_TRUE(p.s.remove(p.b));
    EXPECT_NEAR(p.s.startOf(1), 5.0, 1e-9);
    EXPECT_NEAR(p.s.fragments()[1].gap, 4.0, 1e-9);
    // Chiudendo lo spazio, C si attacca ad A.
    EXPECT_TRUE(p.s.closeGap(1));
    EXPECT_NEAR(p.s.startOf(1), 1.0, 1e-9);
    EXPECT_FALSE(p.s.closeGap(1));  // non c'è più
    EXPECT_FALSE(p.s.closeGap(7));
    EXPECT_EQ(p.s.gapCount(), 0);
}

TEST(SequenceGaps, SplitCutAndMuteKeepTheGaps)
{
    Sequence s;
    s.insert(-1, 2, 0.0, 1.0);              // 0 .. 1
    const int long_ = s.insert(-1, 2, 0.0, 4.0);
    ASSERT_TRUE(s.moveTo(long_, 3.0));            // spazio 1 .. 3, pezzo 3 .. 7
    EXPECT_EQ(s.split(2.0), 0);             // nello spazio non si divide
    EXPECT_NE(s.split(5.0), 0);             // il pezzo: 3 .. 5 e 5 .. 7
    EXPECT_NEAR(s.startOf(1), 3.0, 1e-9);
    EXPECT_NEAR(s.startOf(2), 5.0, 1e-9);
    EXPECT_EQ(s.gapCount(), 1);

    // Audio: i tempi della sequenza tengono conto dello spazio.
    EXPECT_TRUE(s.muteRange(2.0, 3.5));  // dentro lo spazio non c'è niente da togliere
    ASSERT_EQ(s.mutedRanges().size(), 1u);
    EXPECT_NEAR(s.mutedRanges()[0].start, 3.0, 1e-9);
    EXPECT_NEAR(s.mutedRanges()[0].end, 3.5, 1e-9);
    EXPECT_NEAR(s.fragments()[1].muted[0].end, 0.5, 1e-9);  // nei tempi del file

    // Togliendo dal file tutto il primo pezzo dopo lo spazio, lo spazio passa al pezzo dopo.
    ASSERT_EQ(s.removeSourceRange(2, 0.0, 2.0), Sequence::RemoveResult::Removed);
    // Del file 2 resta solo la seconda metà (2 .. 4): il primo pezzo (0 .. 1) è tolto anche lui.
    expectPieces(s, {{2, 2.0, 4.0}});
    EXPECT_NEAR(s.fragments()[0].gap, 2.0, 1e-9);  // lo spazio che c'era prima del pezzo tolto
}

TEST(SequenceGaps, RestoreKeepsGapsAndRejectsNegative)
{
    Fragment a{0, 1, 0.0, 2.0, {}, 1.5};
    Fragment b{0, 1, 2.0, 4.0, {}, -3.0};
    Sequence s;
    s.restore({a, b});
    EXPECT_NEAR(s.startOf(0), 1.5, 1e-9);
    EXPECT_NEAR(s.startOf(1), 3.5, 1e-9);
    EXPECT_EQ(s.gapCount(), 1);
}

}  // namespace
