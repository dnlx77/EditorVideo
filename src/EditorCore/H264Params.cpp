#include "EditorCore/H264Params.h"

#include "EditorCore/NalUnits.h"

#include <algorithm>
#include <stdexcept>

namespace editor::core {

namespace {

// Dati malformati: si smette di leggere e i parametri risultano non validi.
struct ParseError
{
};

// Lettura bit per bit di un RBSP (NAL senza i byte di prevenzione dell'emulazione).
class BitReader
{
public:
    explicit BitReader(std::vector<std::uint8_t> data)
        : data_(std::move(data))
    {
        // Ultimo bit a 1 = bit di chiusura del RBSP: oltre quello non ci sono più dati.
        lastOne_ = -1;
        for (int i = static_cast<int>(data_.size()) * 8 - 1; i >= 0; --i) {
            if ((data_[i / 8] >> (7 - i % 8)) & 1) {
                lastOne_ = i;
                break;
            }
        }
    }

    unsigned bit()
    {
        if (pos_ >= data_.size() * 8)
            throw ParseError{};
        const unsigned b = (data_[pos_ / 8] >> (7 - pos_ % 8)) & 1;
        ++pos_;
        return b;
    }

    unsigned bits(int n)
    {
        unsigned v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }

    unsigned ue()
    {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31)
                throw ParseError{};
        }
        return ((1u << zeros) - 1) + bits(zeros);
    }

    int se()
    {
        const unsigned v = ue();
        return (v & 1) ? static_cast<int>((v + 1) / 2) : -static_cast<int>(v / 2);
    }

    bool moreRbspData() const { return static_cast<int>(pos_) < lastOne_; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_ = 0;
    int lastOne_ = -1;
};

std::vector<std::uint8_t> toRbsp(const std::vector<std::uint8_t>& nal)
{
    // Si saltano l'intestazione del NAL (1 byte) e i byte 0x03 inseriti dopo due zeri.
    std::vector<std::uint8_t> rbsp;
    int zeros = 0;
    for (std::size_t i = 1; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(nal[i]);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    return rbsp;
}

void skipScalingList(BitReader& r, int size)
{
    int last = 8;
    int next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0)
            next = (last + r.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

bool highProfile(int profile)
{
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139:
    case 134: case 135:
        return true;
    default:
        return false;
    }
}

H264Sps parseSps(const std::vector<std::uint8_t>& nal)
{
    BitReader r(toRbsp(nal));
    H264Sps s;
    s.profileIdc = static_cast<int>(r.bits(8));
    s.constraintFlags = static_cast<int>(r.bits(8));
    s.levelIdc = static_cast<int>(r.bits(8));
    s.id = static_cast<int>(r.ue());
    if (highProfile(s.profileIdc)) {
        s.chromaFormatIdc = static_cast<int>(r.ue());
        if (s.chromaFormatIdc == 3)
            s.separateColourPlane = r.bit() != 0;
        s.bitDepthLuma = 8 + static_cast<int>(r.ue());
        s.bitDepthChroma = 8 + static_cast<int>(r.ue());
        s.qpprimeYZeroTransformBypass = r.bit() != 0;
        s.scalingMatrixPresent = r.bit() != 0;
        if (s.scalingMatrixPresent) {
            for (int i = 0; i < (s.chromaFormatIdc != 3 ? 8 : 12); ++i) {
                if (r.bit())
                    skipScalingList(r, i < 6 ? 16 : 64);
            }
        }
    }
    s.log2MaxFrameNum = 4 + static_cast<int>(r.ue());
    s.picOrderCntType = static_cast<int>(r.ue());
    if (s.picOrderCntType == 0) {
        s.log2MaxPicOrderCntLsb = 4 + static_cast<int>(r.ue());
    } else if (s.picOrderCntType == 1) {
        s.deltaPicOrderAlwaysZero = r.bit() != 0;
        s.offsetForNonRefPic = r.se();
        s.offsetForTopToBottomField = r.se();
        const unsigned cycle = r.ue();
        if (cycle > 255)
            throw ParseError{};
        for (unsigned i = 0; i < cycle; ++i)
            s.offsetForRefFrame.push_back(r.se());
    }
    s.maxNumRefFrames = static_cast<int>(r.ue());
    s.gapsInFrameNumAllowed = r.bit() != 0;
    s.widthInMbs = 1 + static_cast<int>(r.ue());
    s.heightInMapUnits = 1 + static_cast<int>(r.ue());
    s.frameMbsOnly = r.bit() != 0;
    if (!s.frameMbsOnly)
        s.mbAdaptiveFrameField = r.bit() != 0;
    s.direct8x8Inference = r.bit() != 0;
    if (r.bit()) {
        s.cropLeft = static_cast<int>(r.ue());
        s.cropRight = static_cast<int>(r.ue());
        s.cropTop = static_cast<int>(r.ue());
        s.cropBottom = static_cast<int>(r.ue());
    }
    s.vuiPresent = r.bit() != 0;
    if (s.vuiPresent && r.bit()) {  // aspect_ratio_info_present_flag
        static const int sar[][2] = {{0, 0},   {1, 1},   {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11},
                                     {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99},
                                     {4, 3},   {3, 2},   {2, 1}};
        const unsigned idc = r.bits(8);
        if (idc == 255) {
            s.sarWidth = static_cast<int>(r.bits(16));
            s.sarHeight = static_cast<int>(r.bits(16));
        } else if (idc < std::size(sar)) {
            s.sarWidth = sar[idc][0];
            s.sarHeight = sar[idc][1];
        }
    }
    // Il resto della VUI (tempi, bitrate, HRD...) non serve a decodificare.
    return s;
}

H264Pps parsePps(const std::vector<std::uint8_t>& nal, const std::vector<H264Sps>& spsList)
{
    BitReader r(toRbsp(nal));
    H264Pps p;
    p.id = static_cast<int>(r.ue());
    p.spsId = static_cast<int>(r.ue());
    p.entropyCodingCabac = r.bit() != 0;
    p.bottomFieldPicOrderInFramePresent = r.bit() != 0;
    p.numSliceGroups = 1 + static_cast<int>(r.ue());
    if (p.numSliceGroups > 1)
        return p;  // gruppi di slice (rarissimi): si confrontano i byte
    p.numRefIdxL0DefaultActive = 1 + static_cast<int>(r.ue());
    p.numRefIdxL1DefaultActive = 1 + static_cast<int>(r.ue());
    p.weightedPred = r.bit() != 0;
    p.weightedBipredIdc = static_cast<int>(r.bits(2));
    p.picInitQp = 26 + r.se();
    p.picInitQs = 26 + r.se();
    p.chromaQpIndexOffset = r.se();
    p.deblockingFilterControlPresent = r.bit() != 0;
    p.constrainedIntraPred = r.bit() != 0;
    p.redundantPicCntPresent = r.bit() != 0;
    p.secondChromaQpIndexOffset = p.chromaQpIndexOffset;
    if (r.moreRbspData()) {
        p.transform8x8Mode = r.bit() != 0;
        p.picScalingMatrixPresent = r.bit() != 0;
        if (p.picScalingMatrixPresent) {
            int chroma = 1;
            for (const H264Sps& s : spsList) {
                if (s.id == p.spsId)
                    chroma = s.chromaFormatIdc;
            }
            const int lists = 6 + (p.transform8x8Mode ? (chroma != 3 ? 2 : 6) : 0);
            for (int i = 0; i < lists; ++i) {
                if (r.bit())
                    skipScalingList(r, i < 6 ? 16 : 64);
            }
        }
        p.secondChromaQpIndexOffset = r.se();
    }
    return p;
}

std::string profileName(int profile)
{
    switch (profile) {
    case 66: return "Baseline";
    case 77: return "Main";
    case 88: return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    default: return std::to_string(profile);
    }
}

std::string chromaName(int idc)
{
    switch (idc) {
    case 0: return "bianco e nero";
    case 1: return "4:2:0";
    case 2: return "4:2:2";
    case 3: return "4:4:4";
    default: return std::to_string(idc);
    }
}

}  // namespace

H264Config parseH264Config(const std::vector<std::uint8_t>& extradata)
{
    H264Config config;
    try {
        std::vector<nal::Nal> parameterNals;
        if (extradata.size() >= 7 && extradata[0] == 1) {
            // avcC
            auto sets = nal::readAvcC(extradata);
            if (!sets)
                throw ParseError{};
            config.nalLengthSize = sets->lengthSize;
            parameterNals = std::move(sets->nals);
        } else {
            // Annex B: NAL separati dai codici di inizio 00 00 01 / 00 00 00 01.
            config.nalLengthSize = 0;
            parameterNals = nal::splitAnnexB(extradata.data(), extradata.size());
        }
        for (nal::Nal& unit : parameterNals) {
            const int type = unit[0] & 0x1f;
            if (type == 7)
                config.rawSps.push_back(std::move(unit));
            else if (type == 8)
                config.rawPps.push_back(std::move(unit));
        }
        if (config.rawSps.empty() || config.rawPps.empty())
            throw ParseError{};
        for (const auto& nal : config.rawSps)
            config.sps.push_back(parseSps(nal));
        for (const auto& nal : config.rawPps)
            config.pps.push_back(parsePps(nal, config.sps));
        config.valid = true;
    } catch (const ParseError&) {
        config = H264Config{};
    }
    return config;
}

std::vector<std::string> compareH264(const H264Config& other, const H264Config& reference)
{
    std::vector<std::string> out;
    const auto add = [&out](const std::string& text) {
        if (std::find(out.begin(), out.end(), text) == out.end())
            out.push_back(text);
    };
    if (!other.valid || !reference.valid) {
        add("parametri di codifica video diversi (non leggibili)");
        return out;
    }
    const std::string encoder = "impostazioni dell'encoder diverse";

    for (const H264Sps& s : other.sps) {
        const auto ref = std::find_if(reference.sps.begin(), reference.sps.end(), [&](const H264Sps& r) { return r.id == s.id; });
        if (ref == reference.sps.end()) {
            add(encoder + " (identificativi dei parametri)");
            continue;
        }
        const H264Sps& r = *ref;
        if (s.profileIdc != r.profileIdc)
            add("profilo H.264 " + profileName(s.profileIdc) + " invece di " + profileName(r.profileIdc));
        if (s.chromaFormatIdc != r.chromaFormatIdc || s.separateColourPlane != r.separateColourPlane)
            add("formato colore " + chromaName(s.chromaFormatIdc) + " invece di " + chromaName(r.chromaFormatIdc));
        if (s.bitDepthLuma != r.bitDepthLuma || s.bitDepthChroma != r.bitDepthChroma)
            add("profondità colore " + std::to_string(s.bitDepthLuma) + " bit invece di " + std::to_string(r.bitDepthLuma)
                + " bit");
        if (s.widthInMbs != r.widthInMbs || s.heightInMapUnits != r.heightInMapUnits || s.cropLeft != r.cropLeft
            || s.cropRight != r.cropRight || s.cropTop != r.cropTop || s.cropBottom != r.cropBottom)
            add("dimensioni codificate diverse");
        if (s.frameMbsOnly != r.frameMbsOnly || s.mbAdaptiveFrameField != r.mbAdaptiveFrameField)
            add("uno è interlacciato e l'altro no");
        if (s.log2MaxFrameNum != r.log2MaxFrameNum || s.gapsInFrameNumAllowed != r.gapsInFrameNumAllowed)
            add(encoder + " (numerazione dei fotogrammi)");
        if (s.picOrderCntType != r.picOrderCntType || s.log2MaxPicOrderCntLsb != r.log2MaxPicOrderCntLsb
            || s.deltaPicOrderAlwaysZero != r.deltaPicOrderAlwaysZero || s.offsetForNonRefPic != r.offsetForNonRefPic
            || s.offsetForTopToBottomField != r.offsetForTopToBottomField || s.offsetForRefFrame != r.offsetForRefFrame)
            add(encoder + " (ordine dei fotogrammi)");
        if (s.maxNumRefFrames > r.maxNumRefFrames)
            add(encoder + " (" + std::to_string(s.maxNumRefFrames) + " fotogrammi di riferimento invece di "
                + std::to_string(r.maxNumRefFrames) + ")");
        if (s.direct8x8Inference != r.direct8x8Inference || s.qpprimeYZeroTransformBypass != r.qpprimeYZeroTransformBypass)
            add(encoder + " (predizione)");
        if (s.scalingMatrixPresent || r.scalingMatrixPresent) {
            // Con le matrici di quantizzazione si confrontano i byte, livello e vincoli esclusi.
            const auto& a = other.rawSps[static_cast<std::size_t>(&s - other.sps.data())];
            const auto& b = reference.rawSps[static_cast<std::size_t>(ref - reference.sps.begin())];
            if (a.size() != b.size() || !std::equal(a.begin() + 4, a.end(), b.begin() + 4) || a[1] != b[1])
                add(encoder + " (matrici di quantizzazione)");
        }
        if (s.sarWidth * r.sarHeight != r.sarWidth * s.sarHeight && (s.sarWidth != 0 || r.sarWidth != 0))
            add("proporzioni dei pixel diverse");
    }

    for (const H264Pps& p : other.pps) {
        const auto ref = std::find_if(reference.pps.begin(), reference.pps.end(), [&](const H264Pps& r) { return r.id == p.id; });
        if (ref == reference.pps.end() || ref->spsId != p.spsId) {
            add(encoder + " (identificativi dei parametri)");
            continue;
        }
        const H264Pps& r = *ref;
        if (p.entropyCodingCabac != r.entropyCodingCabac)
            add(encoder + std::string(" (") + (p.entropyCodingCabac ? "CABAC invece di CAVLC" : "CAVLC invece di CABAC")
                + ")");
        if (p.numSliceGroups != r.numSliceGroups || p.numSliceGroups > 1) {
            const auto& a = other.rawPps[static_cast<std::size_t>(&p - other.pps.data())];
            const auto& b = reference.rawPps[static_cast<std::size_t>(ref - reference.pps.begin())];
            if (a != b)
                add(encoder + " (gruppi di slice)");
        }
        if (p.bottomFieldPicOrderInFramePresent != r.bottomFieldPicOrderInFramePresent
            || p.numRefIdxL0DefaultActive != r.numRefIdxL0DefaultActive
            || p.numRefIdxL1DefaultActive != r.numRefIdxL1DefaultActive || p.weightedPred != r.weightedPred
            || p.weightedBipredIdc != r.weightedBipredIdc || p.picInitQp != r.picInitQp || p.picInitQs != r.picInitQs
            || p.chromaQpIndexOffset != r.chromaQpIndexOffset || p.secondChromaQpIndexOffset != r.secondChromaQpIndexOffset
            || p.deblockingFilterControlPresent != r.deblockingFilterControlPresent
            || p.constrainedIntraPred != r.constrainedIntraPred || p.redundantPicCntPresent != r.redundantPicCntPresent
            || p.transform8x8Mode != r.transform8x8Mode || p.picScalingMatrixPresent != r.picScalingMatrixPresent)
            add(encoder + " (parametri delle immagini)");
    }
    if (other.nalLengthSize != reference.nalLengthSize)
        add(encoder + " (formato dei pacchetti)");
    return out;
}

int maxH264Level(const H264Config& config)
{
    int level = 0;
    for (const H264Sps& s : config.sps)
        level = std::max(level, s.levelIdc);
    return level;
}

std::vector<std::uint8_t> withH264Level(const std::vector<std::uint8_t>& extradata, int level)
{
    std::vector<std::uint8_t> out = extradata;
    if (out.size() < 7 || out[0] != 1 || level <= 0 || level > 255)
        return out;
    out[3] = static_cast<std::uint8_t>(level);
    std::size_t pos = 6;
    for (int i = 0; i < (out[5] & 0x1f) && pos + 2 <= out.size(); ++i) {
        const std::size_t length = (out[pos] << 8) | out[pos + 1];
        if (pos + 2 + 3 < out.size() && length > 3)
            out[pos + 2 + 3] = static_cast<std::uint8_t>(level);  // NAL: intestazione, profilo, vincoli, livello
        pos += 2 + length;
    }
    return out;
}

std::string describeH264(const H264Config& config)
{
    if (!config.valid)
        return "H.264: parametri non leggibili";
    std::string text;
    for (const H264Sps& s : config.sps) {
        int width = s.widthInMbs * 16;
        int height = s.heightInMapUnits * 16 * (s.frameMbsOnly ? 1 : 2);
        width -= 2 * (s.cropLeft + s.cropRight);
        height -= 2 * (s.cropTop + s.cropBottom) * (s.frameMbsOnly ? 1 : 2);
        text += "SPS " + std::to_string(s.id) + ": profilo " + profileName(s.profileIdc) + ", livello "
              + std::to_string(s.levelIdc / 10) + "." + std::to_string(s.levelIdc % 10) + ", " + std::to_string(width)
              + "x" + std::to_string(height) + ", " + chromaName(s.chromaFormatIdc) + " "
              + std::to_string(s.bitDepthLuma) + " bit, " + std::to_string(s.maxNumRefFrames)
              + " fotogrammi di riferimento, frame_num " + std::to_string(s.log2MaxFrameNum) + " bit, POC tipo "
              + std::to_string(s.picOrderCntType) + (s.picOrderCntType == 0 ? " (" + std::to_string(s.log2MaxPicOrderCntLsb) + " bit)" : "")
              + (s.frameMbsOnly ? ", progressivo" : ", interlacciato") + (s.vuiPresent ? ", VUI" : "")
              + (s.scalingMatrixPresent ? ", matrici" : "") + "\n";
    }
    for (const H264Pps& p : config.pps) {
        text += "PPS " + std::to_string(p.id) + " (SPS " + std::to_string(p.spsId) + "): "
              + (p.entropyCodingCabac ? "CABAC" : "CAVLC") + ", rif. predefiniti " + std::to_string(p.numRefIdxL0DefaultActive)
              + "/" + std::to_string(p.numRefIdxL1DefaultActive) + ", pesi " + (p.weightedPred ? "sì" : "no") + "/"
              + std::to_string(p.weightedBipredIdc) + ", QP iniziale " + std::to_string(p.picInitQp) + ", offset croma "
              + std::to_string(p.chromaQpIndexOffset) + "/" + std::to_string(p.secondChromaQpIndexOffset)
              + ", 8x8 " + (p.transform8x8Mode ? "sì" : "no") + ", deblocking " + (p.deblockingFilterControlPresent ? "sì" : "no")
              + "\n";
    }
    return text;
}

}  // namespace editor::core
