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
    const std::vector<double> starts = p.s.starts();  // tutti gli inizi in una volta, come startOf
    ASSERT_EQ(starts.size(), 3u);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(starts[static_cast<std::size_t>(i)], p.s.startOf(i), 1e-9);
    EXPECT_NEAR(p.s.totalLength(), 7.0, 1e-9);
    EXPECT_EQ(p.s.gapCount(), 1);
    EXPECT_EQ(p.s.indexAt(4.5), -1);    // nello spazio non c'è nessun pezzo
    EXPECT_EQ(p.s.gapIndexAt(4.5), 2);  // è lo spazio prima di C
    EXPECT_EQ(p.s.gapIndexAt(0.5), -1);
    EXPECT_EQ(p.s.indexAt(6.5), 2);

    // Verso sinistra un pezzo passa in un altro spazio senza muovere gli altri: C (1 s) davanti a B... non ci sta (B 1 .. 3),
    // ma nello spazio 3 .. 6 sì, e lì B resta dov'è.
    EXPECT_TRUE(p.s.moveTo(p.c, 4.0));
    EXPECT_NEAR(p.s.startOf(1), 1.0, 1e-9);
    EXPECT_NEAR(p.s.startOf(2), 4.0, 1e-9);
}

TEST(SequenceGaps, MovingForwardPushesThePiecesAfter)
{
    ThreePieces p;  // A 0 .. 1, B 1 .. 3, C 3 .. 4
    std::vector<std::pair<int, double>> shifts;
    EXPECT_TRUE(p.s.moveTo(p.a, 0.5, &shifts));  // A 0,5 .. 1,5 urta B: B e C scorrono di 0,5
    expectPieces(p.s, {{2, 0.0, 1.0}, {2, 2.0, 4.0}, {2, 5.0, 6.0}});
    EXPECT_NEAR(p.s.startOf(0), 0.5, 1e-9);
    EXPECT_NEAR(p.s.startOf(1), 1.5, 1e-9);
    EXPECT_NEAR(p.s.startOf(2), 3.5, 1e-9);
    EXPECT_EQ(p.s.gapCount(), 1);  // solo lo spazio prima di A
    ASSERT_EQ(shifts.size(), 3u);
    for (const auto& [id, delta] : shifts)
        EXPECT_NEAR(delta, 0.5, 1e-9);

    // Uno spazio vuoto dopo il pezzo assorbe la spinta: i pezzi dopo lo spazio non si muovono finché non ci arriva.
    ThreePieces q;
    ASSERT_TRUE(q.s.moveTo(q.c, 6.0));  // A 0 .. 1, B 1 .. 3, spazio, C 6 .. 7
    EXPECT_TRUE(q.s.moveTo(q.a, 1.0));  // A 1 .. 2 tocca B (1 .. 3), che scorre a 2 .. 4; C resta
    EXPECT_NEAR(q.s.startOf(1), 2.0, 1e-9);
    EXPECT_NEAR(q.s.startOf(2), 6.0, 1e-9);
    EXPECT_TRUE(q.s.moveTo(q.a, 4.5));  // A 4,5 .. 5,5: B spinta a 5,5 .. 7,5, C a 7,5
    EXPECT_NEAR(q.s.startOf(1), 5.5, 1e-9);
    EXPECT_NEAR(q.s.startOf(2), 7.5, 1e-9);
    // Verso destra non si può andare prima della fine del pezzo che sta prima (qui nessuno): niente da fermare.
    EXPECT_TRUE(p.s.moveTo(p.b, 20.0));
}

TEST(SequenceGaps, WithoutPushingAPieceStopsAgainstTheNext)
{
    ThreePieces p;  // A 0 .. 1, B 1 .. 3, C 3 .. 4 (in tempo di sequenza)
    EXPECT_FALSE(p.s.moveTo(p.a, 0.5, nullptr, false));  // urterebbe B: niente spinta, niente spostamento
    EXPECT_NEAR(p.s.startOf(1), 1.0, 1e-9);
    EXPECT_NEAR(p.s.nearestFreeStart(p.a, 2.5, false), 4.0, 1e-9);  // dopo C, lo spazio libero più vicino
    ThreePieces q;
    ASSERT_TRUE(q.s.moveTo(q.c, 6.0));  // C 6 .. 7, spazio fra B e C
    // Senza spinta A può saltare oltre B, nello spazio dopo di lui, e B non si muove.
    EXPECT_TRUE(q.s.moveTo(q.a, 4.0, nullptr, false));
    EXPECT_NEAR(q.s.startOf(0), 1.0, 1e-9);  // B è rimasto dov'era
    EXPECT_NEAR(q.s.startOf(1), 4.0, 1e-9);  // A è dopo di lui
}

TEST(SequenceGaps, PiecesNeverOverlap)
{
    ThreePieces p;
    EXPECT_FALSE(p.s.moveTo(p.c, 2.5));  // su B (verso sinistra non si spinge)
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
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 5.2), 5.2, 1e-9);  // verso destra spinge C: nessun limite
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 0.0), 1.0, 1e-9);  // verso sinistra si ferma contro A
    EXPECT_NEAR(p.s.nearestFreeStart(p.b, 6.6), 6.6, 1e-9);
    // A (1 s) trascinata verso destra: spinge B (nessun limite, salvo la fine del pezzo prima: nessuno).
    EXPECT_NEAR(p.s.nearestFreeStart(p.a, 2.5), 2.5, 1e-9);
    EXPECT_NEAR(p.s.nearestFreeStart(p.a, 1.2), 1.2, 1e-9);
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

// Bordi dei pezzi (step 16). Sequenza di prova: A = file 1 [2, 5) da 0, spazio di 1 s, B = file 2 [10, 14) da 4.
Sequence trimSample(int& a, int& b)
{
    Sequence s;
    a = s.insert(-1, 1, 2.0, 5.0);
    b = s.insert(-1, 2, 10.0, 14.0);
    EXPECT_TRUE(s.moveTo(b, 4.0));
    return s;
}

TEST(SequenceTrim, ShorteningLeavesAGapAndTheOthersStill)
{
    int a = 0, b = 0;
    Sequence s = trimSample(a, b);
    EXPECT_TRUE(s.trim(a, Sequence::Edge::End, 4.0, 20.0, 0.04, false));   // fine di A 1 s prima
    EXPECT_TRUE(s.trim(b, Sequence::Edge::Start, 11.0, 20.0, 0.04, false)); // inizio di B 1 s dopo
    expectPieces(s, {{1, 2.0, 4.0}, {2, 11.0, 14.0}});
    EXPECT_NEAR(s.startOf(0), 0.0, 1e-9);
    EXPECT_NEAR(s.startOf(1), 5.0, 1e-9);  // la fine di B resta a 8
    EXPECT_NEAR(s.totalLength(), 8.0, 1e-9);
}

TEST(SequenceTrim, LengtheningStopsAgainstTheNeighbourAndTheFile)
{
    int a = 0, b = 0;
    Sequence s = trimSample(a, b);
    // A può allungarsi solo fino all'inizio di B (1 s di spazio).
    EXPECT_NEAR(s.trimLimit(a, Sequence::Edge::End, 9.0, 20.0, 0.04, false), 6.0, 1e-9);
    // B all'indietro solo fino alla fine di A.
    EXPECT_NEAR(s.trimLimit(b, Sequence::Edge::Start, 0.0, 20.0, 0.04, false), 9.0, 1e-9);
    // A all'indietro: è già all'inizio della sequenza, non può andare prima; con ripple sì, fino all'inizio del file.
    EXPECT_NEAR(s.trimLimit(a, Sequence::Edge::Start, -5.0, 20.0, 0.04, false), 2.0, 1e-9);
    EXPECT_NEAR(s.trimLimit(a, Sequence::Edge::Start, -5.0, 20.0, 0.04, true), 0.0, 1e-9);
    // B in avanti: fino alla fine del file.
    EXPECT_NEAR(s.trimLimit(b, Sequence::Edge::End, 30.0, 20.0, 0.04, false), 20.0, 1e-9);
    // Mai sotto un fotogramma.
    EXPECT_NEAR(s.trimLimit(a, Sequence::Edge::Start, 4.99, 20.0, 0.04, false), 4.96, 1e-9);
    EXPECT_NEAR(s.trimLimit(b, Sequence::Edge::End, 0.0, 20.0, 0.04, false), 10.04, 1e-9);

    EXPECT_TRUE(s.trim(a, Sequence::Edge::End, 9.0, 20.0, 0.04, false));
    EXPECT_TRUE(s.trim(b, Sequence::Edge::End, 30.0, 20.0, 0.04, false));
    expectPieces(s, {{1, 2.0, 6.0}, {2, 10.0, 20.0}});
    EXPECT_NEAR(s.startOf(1), 4.0, 1e-9);  // B non si è mosso
    EXPECT_EQ(s.gapCount(), 0);
    EXPECT_FALSE(s.trim(a, Sequence::Edge::End, 7.0, 20.0, 0.04, false));  // già contro B
}

TEST(SequenceTrim, RippleMovesTheFollowingPieces)
{
    int a = 0, b = 0;
    Sequence s = trimSample(a, b);
    // Con ripple A si allunga di 3 s oltre lo spazio: B scorre avanti, lo spazio resta.
    EXPECT_TRUE(s.trim(a, Sequence::Edge::End, 8.0, 20.0, 0.04, true));
    EXPECT_NEAR(s.startOf(1), 7.0, 1e-9);
    // L'inizio di B un secondo prima: B resta dove inizia e si allunga, niente scorre indietro.
    EXPECT_TRUE(s.trim(b, Sequence::Edge::Start, 9.0, 20.0, 0.04, true));
    EXPECT_NEAR(s.startOf(1), 7.0, 1e-9);
    expectPieces(s, {{1, 2.0, 8.0}, {2, 9.0, 14.0}});
    EXPECT_NEAR(s.totalLength(), 12.0, 1e-9);
    // Accorciando l'inizio di A con ripple, B torna indietro.
    EXPECT_TRUE(s.trim(a, Sequence::Edge::Start, 4.0, 20.0, 0.04, true));
    EXPECT_NEAR(s.startOf(1), 5.0, 1e-9);
}

TEST(SequenceTrim, MutedRangesOutsideThePieceAreCut)
{
    int a = 0, b = 0;
    Sequence s = trimSample(a, b);
    ASSERT_TRUE(s.muteRange(0.5, 2.5));  // nel file di A: [2.5, 4.5)
    EXPECT_TRUE(s.trim(a, Sequence::Edge::End, 3.5, 20.0, 0.04, false));
    ASSERT_EQ(s.fragments()[0].muted.size(), 1u);
    EXPECT_NEAR(s.fragments()[0].muted[0].start, 2.5, 1e-9);
    EXPECT_NEAR(s.fragments()[0].muted[0].end, 3.5, 1e-9);
    EXPECT_TRUE(s.trim(a, Sequence::Edge::Start, 3.5 - 0.04, 20.0, 0.04, false));
    ASSERT_EQ(s.fragments()[0].muted.size(), 1u);
    EXPECT_NEAR(s.fragments()[0].muted[0].start, 3.46, 1e-9);
}

}  // namespace

TEST(SequenceEnvelope, LineFollowsThePieceAndSplitsWithIt)
{
    Sequence s;
    const int id = s.insert(-1, 1, 2.0, 8.0);
    EXPECT_TRUE(s.setEnvelope(id, {{6.0, -20.0}, {3.0, 0.0}}));  // tempi del file, messi in ordine
    EXPECT_FALSE(s.setEnvelope(id, {{3.0, 0.0}, {6.0, -20.0}}));
    EXPECT_FALSE(s.setEnvelope(999, {}));
    ASSERT_EQ(s.find(id)->envelope.size(), 2u);
    EXPECT_DOUBLE_EQ(s.find(id)->envelope[0].time, 3.0);

    const int second = s.split(3.0);  // secondo 5 del file
    ASSERT_NE(second, 0);
    EXPECT_EQ(s.find(id)->envelope, s.find(second)->envelope);

    // Togliendo un tratto di sequenza le parti che restano tengono la linea.
    EXPECT_EQ(s.removeSourceRange(1, 3.0, 4.0), Sequence::RemoveResult::Removed);
    for (const Fragment& f : s.fragments())
        EXPECT_EQ(f.envelope.size(), 2u);
}

