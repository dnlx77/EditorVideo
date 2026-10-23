#include "EditorCore/NalUnits.h"

#include <string>

namespace editor::core::nal {

namespace {

// Legge da `pos` una lista di NAL preceduti da 2 byte di lunghezza; `keep` decide quali tenere (dato il primo byte).
// false se i dati finiscono prima del previsto o un NAL è vuoto.
template <typename Keep>
bool takeNal(const std::vector<std::uint8_t>& extra, std::size_t& pos, std::vector<Nal>& out, Keep keep)
{
    if (pos + 2 > extra.size())
        return false;
    const std::size_t length = static_cast<std::size_t>(extra[pos] << 8 | extra[pos + 1]);
    pos += 2;
    if (length == 0 || pos + length > extra.size())
        return false;
    if (keep(extra[pos]))
        out.emplace_back(extra.begin() + static_cast<std::ptrdiff_t>(pos), extra.begin() + static_cast<std::ptrdiff_t>(pos + length));
    pos += length;
    return true;
}

}  // namespace

std::vector<Nal> splitAnnexB(const std::uint8_t* data, std::size_t size)
{
    std::vector<Nal> nals;
    const auto add = [&](std::size_t begin, std::size_t end) {
        while (end > begin && data[end - 1] == 0)
            --end;  // gli zeri prima del codice di inizio successivo non fanno parte del NAL
        if (end > begin)
            nals.emplace_back(data + begin, data + end);
    };
    std::size_t begin = std::string::npos;
    std::size_t i = 0;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (begin != std::string::npos)
                add(begin, i);
            i += 3;
            begin = i;
            continue;
        }
        ++i;
    }
    if (begin != std::string::npos && begin < size)
        add(begin, size);
    return nals;
}

bool startsWithStartCode(const std::uint8_t* data, std::size_t size)
{
    return (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        || (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

std::optional<ParameterSets> readAvcC(const std::vector<std::uint8_t>& extra)
{
    if (extra.size() < 7 || extra[0] != 1)
        return std::nullopt;
    ParameterSets out;
    out.lengthSize = (extra[4] & 3) + 1;
    std::size_t pos = 5;
    for (int list = 0; list < 2; ++list) {  // prima gli SPS, poi i PPS
        if (pos >= extra.size())
            return std::nullopt;
        const int count = list == 0 ? (extra[pos++] & 0x1f) : extra[pos++];
        for (int i = 0; i < count; ++i) {
            if (!takeNal(extra, pos, out.nals, [](std::uint8_t) { return true; }))
                return std::nullopt;
        }
    }
    return out;
}

std::optional<ParameterSets> readHvcC(const std::vector<std::uint8_t>& extra)
{
    if (extra.size() < 23 || extra[0] != 1)
        return std::nullopt;
    ParameterSets out;
    out.lengthSize = (extra[21] & 3) + 1;
    const int arrays = extra[22];
    std::size_t pos = 23;
    for (int a = 0; a < arrays; ++a) {
        if (pos + 3 > extra.size())
            return std::nullopt;
        const int type = extra[pos] & 0x3f;
        const std::size_t count = static_cast<std::size_t>(extra[pos + 1] << 8 | extra[pos + 2]);
        pos += 3;
        const bool keep = type == 32 || type == 33 || type == 34;  // VPS, SPS, PPS
        for (std::size_t i = 0; i < count; ++i) {
            if (!takeNal(extra, pos, out.nals, [keep](std::uint8_t) { return keep; }))
                return std::nullopt;
        }
    }
    return out;
}

void appendLengthPrefixed(std::vector<std::uint8_t>& out, const std::uint8_t* nal, std::size_t size, int lengthSize)
{
    for (int b = lengthSize - 1; b >= 0; --b)
        out.push_back(static_cast<std::uint8_t>(size >> (8 * b)));
    out.insert(out.end(), nal, nal + size);
}

}  // namespace editor::core::nal
