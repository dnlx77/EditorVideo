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

}  // namespace editor::core::nal
