#include "EditorCore/Sequence.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace editor::core;

namespace {

// File 1: fotogrammi chiave ogni 2 s; file 2: ogni secondo; file 3: nessuno (solo audio).
double snap(int clipId, double t)
{
    if (clipId == 3)
        return t;
    const double step = clipId == 1 ? 2.0 : 1.0;
    return std::floor(t / step + 1e-6) * step;
}

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

TEST(Sequence, InsertSnapsStartAndAcceptsAnyOrderAndRepeats)
{
    Sequence s;
    EXPECT_NE(s.insert(-1, 1, 4.7, 6.0, snap), 0);  // -> 4.0 .. 6.0
    EXPECT_NE(s.insert(-1, 2, 0.5, 1.5, snap), 0);  // -> 0.0 .. 1.5
    EXPECT_NE(s.insert(-1, 1, 0.0, 1.0, snap), 0);  // lo stesso file di nuovo, prima nel tempo
    expectPieces(s, {{1, 4.0, 6.0}, {2, 0.0, 1.5}, {1, 0.0, 1.0}});
    EXPECT_NEAR(s.totalLength(), 4.5, 1e-9);
}

TEST(Sequence, InsertAtPosition)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 2.0, snap);
    s.insert(-1, 1, 4.0, 6.0, snap);
    s.insert(1, 2, 3.0, 3.5, snap);   // in mezzo
    s.insert(0, 2, 5.0, 6.0, snap);   // in testa
    s.insert(99, 3, 1.0, 2.0, snap);  // oltre la fine: in fondo
    expectPieces(s, {{2, 5.0, 6.0}, {1, 0.0, 2.0}, {2, 3.0, 3.5}, {1, 4.0, 6.0}, {3, 1.0, 2.0}});
}

TEST(Sequence, EmptyOrInvertedPieceIsRejected)
{
    Sequence s;
    EXPECT_EQ(s.insert(-1, 1, 3.0, 3.0, snap), 0);
    EXPECT_EQ(s.insert(-1, 1, 3.0, 2.0, snap), 0);
    EXPECT_EQ(s.insert(-1, 1, 3.0, 3.0005, snap), 0);  // l'aggancio lo allungherebbe all'indietro: rifiutato
    EXPECT_TRUE(s.empty());
}

TEST(Sequence, PositionsInTheSequence)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 2.0, snap);  // 0 .. 2
    s.insert(-1, 2, 1.0, 1.5, snap);  // 2 .. 2.5
    s.insert(-1, 1, 4.0, 7.0, snap);  // 2.5 .. 5.5
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
    const int a = s.insert(-1, 1, 0.0, 1.0, snap);
    const int b = s.insert(-1, 2, 0.0, 1.0, snap);
    const int c = s.insert(-1, 3, 0.0, 1.0, snap);
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
    s.insert(-1, 1, 0.0, 1.0, snap);
    s.insert(-1, 2, 0.0, 1.0, snap);
    s.insert(-1, 1, 4.0, 5.0, snap);
    s.removeClip(1);
    expectPieces(s, {{2, 0.0, 1.0}});
}

TEST(Sequence, SplitAtKeyframeBeforePlayhead)
{
    Sequence s;
    s.insert(-1, 2, 0.0, 1.0, snap);  // 0 .. 1 nella sequenza
    s.insert(-1, 1, 0.0, 8.0, snap);  // 1 .. 9 nella sequenza
    // A 5,3 s della sequenza siamo a 4,3 s del file 1: si divide al fotogramma chiave di 4 s.
    const int second = s.split(5.3, snap);
    EXPECT_NE(second, 0);
    expectPieces(s, {{2, 0.0, 1.0}, {1, 0.0, 4.0}, {1, 4.0, 8.0}});
    EXPECT_EQ(s.indexOf(second), 2);
    EXPECT_NEAR(s.totalLength(), 9.0, 1e-9);  // niente perso né ripetuto
}

TEST(Sequence, SplitImpossibleWithoutKeyframeInside)
{
    Sequence s;
    s.insert(-1, 1, 2.0, 3.5, snap);           // nessun fotogramma chiave dopo l'inizio
    EXPECT_EQ(s.split(1.0, snap), 0);
    EXPECT_EQ(s.split(10.0, snap), 0);         // oltre la fine
    expectPieces(s, {{1, 2.0, 3.5}});
}

TEST(Sequence, RemoveSourceRangeFromPieces)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 10.0, snap);
    s.insert(-1, 2, 0.0, 3.0, snap);
    s.insert(-1, 1, 0.0, 10.0, snap);  // lo stesso file due volte: il pezzo tolto sparisce da entrambi
    EXPECT_EQ(s.removeSourceRange(1, 3.0, 6.0, snap), Sequence::RemoveResult::Removed);
    expectPieces(s, {{1, 0.0, 3.0}, {1, 6.0, 10.0}, {2, 0.0, 3.0}, {1, 0.0, 3.0}, {1, 6.0, 10.0}});
}

TEST(Sequence, RemoveSourceRangeReasons)
{
    Sequence s;
    s.insert(-1, 1, 0.0, 4.0, snap);
    EXPECT_EQ(s.removeSourceRange(1, 2.0, 2.0, snap), Sequence::RemoveResult::Empty);
    EXPECT_EQ(s.removeSourceRange(1, 6.0, 8.0, snap), Sequence::RemoveResult::OutsideKept);  // fuori dai pezzi
    EXPECT_EQ(s.removeSourceRange(2, 0.0, 1.0, snap), Sequence::RemoveResult::OutsideKept);  // file non usato
    EXPECT_EQ(s.removeSourceRange(1, 2.5, 3.5, snap), Sequence::RemoveResult::NoKeyframeInside);
    expectPieces(s, {{1, 0.0, 4.0}});
}

}  // namespace
