#include "EditorCore/NalUnits.h"

#include <gtest/gtest.h>

using namespace editor::core::nal;

namespace {

TEST(NalUnits, SplitsAnnexBAndDropsZerosBeforeStartCodes)
{
    // Codice di inizio a 4 byte, un NAL, codice a 3 byte, un NAL che finisce con zeri di riempimento.
    const std::vector<std::uint8_t> stream = {0, 0, 0, 1, 0x67, 0xAA, 0xBB, 0, 0, 1, 0x68, 0xCC, 0, 0, 0, 0, 0, 1, 0x65, 0xDD};
    const std::vector<Nal> nals = splitAnnexB(stream.data(), stream.size());
    ASSERT_EQ(nals.size(), 3u);
    EXPECT_EQ(nals[0], (Nal{0x67, 0xAA, 0xBB}));
    EXPECT_EQ(nals[1], (Nal{0x68, 0xCC}));
    EXPECT_EQ(nals[2], (Nal{0x65, 0xDD}));
    EXPECT_TRUE(startsWithStartCode(stream.data(), stream.size()));
    EXPECT_FALSE(startsWithStartCode(stream.data() + 4, stream.size() - 4));
}

TEST(NalUnits, NoStartCodeMeansNoNals)
{
    const std::vector<std::uint8_t> data = {1, 2, 3, 4};
    EXPECT_TRUE(splitAnnexB(data.data(), data.size()).empty());
    EXPECT_TRUE(splitAnnexB(nullptr, 0).empty());
}

TEST(NalUnits, ReadsAvcC)
{
    // versione, profilo, compatibilità, livello, 0xFC | (lunghezza - 1), 0xE0 | numero di SPS, [2 byte + SPS], numero di PPS...
    const std::vector<std::uint8_t> avcc = {1, 100, 0, 40, 0xFF, 0xE1, 0, 3, 0x67, 1, 2, 1, 0, 2, 0x68, 9};
    const auto sets = readAvcC(avcc);
    ASSERT_TRUE(sets.has_value());
    EXPECT_EQ(sets->lengthSize, 4);
    ASSERT_EQ(sets->nals.size(), 2u);
    EXPECT_EQ(sets->nals[0], (Nal{0x67, 1, 2}));
    EXPECT_EQ(sets->nals[1], (Nal{0x68, 9}));
}

TEST(NalUnits, RejectsTruncatedOrForeignAvcC)
{
    std::vector<std::uint8_t> avcc = {1, 100, 0, 40, 0xFF, 0xE1, 0, 3, 0x67, 1, 2, 1, 0, 2, 0x68, 9};
    avcc.pop_back();  // il PPS dichiara 2 byte ma ne restano 1
    EXPECT_FALSE(readAvcC(avcc).has_value());
    EXPECT_FALSE(readAvcC({0, 0, 0, 1, 0x67, 1, 2}).has_value());  // Annex B
    EXPECT_FALSE(readAvcC({}).has_value());
}

TEST(NalUnits, ReadsHvcCKeepingOnlyParameterSets)
{
    std::vector<std::uint8_t> hvcc(22, 0);
    hvcc[0] = 1;
    hvcc[21] = 0xFF;  // lunghezza su 4 byte
    hvcc.push_back(2);  // due array
    // VPS (tipo 32): un NAL di 3 byte
    hvcc.insert(hvcc.end(), {32, 0, 1, 0, 3, 0x40, 1, 2});
    // SEI (tipo 39): non da tenere
    hvcc.insert(hvcc.end(), {39, 0, 1, 0, 2, 0x4E, 7});
    const auto sets = readHvcC(hvcc);
    ASSERT_TRUE(sets.has_value());
    EXPECT_EQ(sets->lengthSize, 4);
    ASSERT_EQ(sets->nals.size(), 1u);
    EXPECT_EQ(sets->nals[0], (Nal{0x40, 1, 2}));
}

TEST(NalUnits, WritesLengthPrefixes)
{
    std::vector<std::uint8_t> out;
    const std::uint8_t nal[] = {0x65, 1, 2};
    appendLengthPrefixed(out, nal, sizeof(nal), 4);
    appendLengthPrefixed(out, nal, sizeof(nal), 2);
    EXPECT_EQ(out, (std::vector<std::uint8_t>{0, 0, 0, 3, 0x65, 1, 2, 0, 3, 0x65, 1, 2}));
}

}  // namespace
