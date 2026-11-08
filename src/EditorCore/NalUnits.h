#pragma once

// Unità NAL di H.264 e HEVC: lettura dei parametri di codifica dai "dati extra" (avcC / hvcC) e dei flussi Annex B, e
// scrittura con la lunghezza davanti. Interno a EditorCore.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace editor::core::nal {

using Nal = std::vector<std::uint8_t>;

// I parametri di codifica di un file (SPS e PPS, per HEVC anche VPS) e quanti byte misura la lunghezza davanti a ogni
// NAL nei suoi pacchetti.
struct ParameterSets
{
    int lengthSize = 4;
    std::vector<Nal> nals;
};

// NAL di un flusso Annex B (con i codici di inizio 00 00 01 / 00 00 00 01). Gli zeri prima di un codice di inizio non
// fanno parte del NAL; i NAL vuoti si saltano.
std::vector<Nal> splitAnnexB(const std::uint8_t* data, std::size_t size);

bool startsWithStartCode(const std::uint8_t* data, std::size_t size);

// Dai "dati extra" in formato avcC (H.264) o hvcC (HEVC): tutti gli SPS e i PPS (per HEVC anche i VPS). std::nullopt se
// i dati sono in un altro formato (es. Annex B) o non si capiscono.
std::optional<ParameterSets> readAvcC(const std::vector<std::uint8_t>& extra);
std::optional<ParameterSets> readHvcC(const std::vector<std::uint8_t>& extra);

// Aggiunge a `out` il NAL preceduto dalla sua lunghezza su `lengthSize` byte.
void appendLengthPrefixed(std::vector<std::uint8_t>& out, const std::uint8_t* nal, std::size_t size, int lengthSize);

// Lettura bit per bit del contenuto di un NAL (RBSP): salta i primi `headerBytes` byte (l'intestazione: 1 in H.264, 2 in
// HEVC) e i byte di protezione 0x03 dopo due zeri. Oltre la fine dei dati, con PastEnd::Throw si lancia Error, con
// PastEnd::Zeros si leggono zeri (e un numero Exp-Golomb si ferma a 31 zeri).
class RbspReader
{
public:
    struct Error
    {
    };
    enum class PastEnd { Throw, Zeros };

    RbspReader(const Nal& nal, int headerBytes, PastEnd pastEnd);

    unsigned bit();
    unsigned bits(int n);
    unsigned ue();  // Exp-Golomb senza segno
    int se();       // Exp-Golomb con segno
    // Ci sono ancora dati prima del bit di chiusura del RBSP (l'ultimo bit a 1).
    bool moreRbspData() const { return static_cast<long long>(pos_) < lastOne_; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_ = 0;
    long long lastOne_ = -1;
    PastEnd pastEnd_;
};

}  // namespace editor::core::nal
