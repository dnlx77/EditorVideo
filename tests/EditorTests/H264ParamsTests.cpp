#include "EditorCore/Compatibility.h"
#include "EditorCore/H264Params.h"

#include <gtest/gtest.h>

#include <bit>

using namespace editor::core;

namespace {

// Scrittura bit per bit di SPS e PPS H.264 (nei test non c'è un encoder H.264).
class BitWriter
{
public:
    void bit(unsigned b)
    {
        if (count_ % 8 == 0)
            rbsp_.push_back(0);
        if (b)
            rbsp_.back() |= static_cast<std::uint8_t>(0x80 >> (count_ % 8));
        ++count_;
    }
    void bits(unsigned v, int n)
    {
        for (int i = n - 1; i >= 0; --i)
            bit((v >> i) & 1);
    }
    void ue(unsigned v)
    {
        const int length = std::bit_width(v + 1);
        bits(0, length - 1);
        bits(v + 1, length);
    }
    void se(int v) { ue(v > 0 ? static_cast<unsigned>(2 * v - 1) : static_cast<unsigned>(-2 * v)); }

    // NAL completo: intestazione, RBSP con il bit di chiusura e i byte di prevenzione dell'emulazione.
    std::vector<std::uint8_t> nal(std::uint8_t header)
    {
        bit(1);
        while (count_ % 8 != 0)
            bit(0);
        std::vector<std::uint8_t> out = {header};
        int zeros = 0;
        for (std::uint8_t b : rbsp_) {
            if (zeros >= 2 && b <= 3) {
                out.push_back(0x03);
                zeros = 0;
            }
            out.push_back(b);
            zeros = b == 0 ? zeros + 1 : 0;
        }
        return out;
    }

private:
    std::vector<std::uint8_t> rbsp_;
    int count_ = 0;
};

// I parametri di un tipico video 1920x1080 da telefono o da registrazione di gioco.
struct Params
{
    int profile = 100;
    int constraints = 0;
    int level = 40;
    int widthMbs = 120;
    int heightMbs = 68;
    int cropBottom = 4;  // 68*16 = 1088 -> 1080
    int frameNumBits = 8;
    int pocBits = 8;
    int refs = 4;
    bool vui = true;
    unsigned timeScale = 60000;
    unsigned unitsInTick = 1001;
    bool cabac = true;
    int refIdxL0 = 3;
    int initQp = 26;
};

std::vector<std::uint8_t> sps(const Params& p)
{
    BitWriter w;
    w.bits(static_cast<unsigned>(p.profile), 8);
    w.bits(static_cast<unsigned>(p.constraints), 8);
    w.bits(static_cast<unsigned>(p.level), 8);
    w.ue(0);  // id
    if (p.profile >= 100) {
        w.ue(1);  // 4:2:0
        w.ue(0);  // 8 bit
        w.ue(0);
        w.bit(0);
        w.bit(0);  // niente matrici
    }
    w.ue(static_cast<unsigned>(p.frameNumBits - 4));
    w.ue(0);  // POC tipo 0
    w.ue(static_cast<unsigned>(p.pocBits - 4));
    w.ue(static_cast<unsigned>(p.refs));
    w.bit(0);
    w.ue(static_cast<unsigned>(p.widthMbs - 1));
    w.ue(static_cast<unsigned>(p.heightMbs - 1));
    w.bit(1);  // progressivo
    w.bit(1);  // direct 8x8
    w.bit(p.cropBottom > 0 ? 1 : 0);
    if (p.cropBottom > 0) {
        w.ue(0);
        w.ue(0);
        w.ue(0);
        w.ue(static_cast<unsigned>(p.cropBottom));
    }
    w.bit(p.vui ? 1 : 0);
    if (p.vui) {
        w.bit(1);       // proporzioni dei pixel presenti
        w.bits(1, 8);   // 1:1
        w.bit(0);       // overscan
        w.bit(0);       // segnale video
        w.bit(0);       // posizione croma
        w.bit(1);       // tempi presenti
        w.bits(p.unitsInTick, 32);
        w.bits(p.timeScale, 32);
        w.bit(1);
        w.bit(0);       // HRD
        w.bit(0);
        w.bit(0);
        w.bit(0);
    }
    return w.nal(0x67);
}

std::vector<std::uint8_t> pps(const Params& p)
{
    BitWriter w;
    w.ue(0);  // id
    w.ue(0);  // SPS
    w.bit(p.cabac ? 1 : 0);
    w.bit(0);
    w.ue(0);  // un gruppo di slice
    w.ue(static_cast<unsigned>(p.refIdxL0 - 1));
    w.ue(0);
    w.bit(1);       // predizione pesata
    w.bits(2, 2);
    w.se(p.initQp - 26);
    w.se(0);
    w.se(-2);
    w.bit(1);
    w.bit(0);
    w.bit(0);
    w.bit(1);  // 8x8
    w.bit(0);
    w.se(-2);
    return w.nal(0x68);
}

std::vector<std::uint8_t> avcC(const Params& p)
{
    const auto s = sps(p);
    const auto q = pps(p);
    std::vector<std::uint8_t> out = {1, static_cast<std::uint8_t>(p.profile), static_cast<std::uint8_t>(p.constraints),
                                     static_cast<std::uint8_t>(p.level), 0xff, 0xe1};
    out.push_back(static_cast<std::uint8_t>(s.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(1);
    out.push_back(static_cast<std::uint8_t>(q.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(q.size()));
    out.insert(out.end(), q.begin(), q.end());
    return out;
}

std::vector<std::string> compareMessages(const H264Config& other, const H264Config& reference)
{
    std::vector<std::string> messages;
    for (const H264Difference& difference : findH264Differences(other, reference))
        messages.push_back(difference.message);
    return messages;
}

std::vector<std::string> differences(const Params& other, const Params& reference = {})
{
    return compareMessages(parseH264Config(avcC(other)), parseH264Config(avcC(reference)));
}

TEST(H264Params, ReadsSpsAndPps)
{
    const H264Config c = parseH264Config(avcC({}));
    ASSERT_TRUE(c.valid);
    ASSERT_EQ(c.sps.size(), 1u);
    ASSERT_EQ(c.pps.size(), 1u);
    EXPECT_EQ(c.nalLengthSize, 4);
    const H264Sps& s = c.sps[0];
    EXPECT_EQ(s.profileIdc, 100);
    EXPECT_EQ(s.levelIdc, 40);
    EXPECT_EQ(s.widthInMbs, 120);
    EXPECT_EQ(s.heightInMapUnits, 68);
    EXPECT_EQ(s.cropBottom, 4);
    EXPECT_EQ(s.log2MaxFrameNum, 8);
    EXPECT_EQ(s.maxNumRefFrames, 4);
    EXPECT_EQ(s.sarWidth, 1);
    const H264Pps& p = c.pps[0];
    EXPECT_TRUE(p.entropyCodingCabac);
    EXPECT_EQ(p.numRefIdxL0DefaultActive, 3);
    EXPECT_TRUE(p.transform8x8Mode);
    EXPECT_EQ(p.secondChromaQpIndexOffset, -2);
    EXPECT_NE(describeH264(c).find("1920x1080"), std::string::npos) << describeH264(c);
}

TEST(H264Params, EmulationPreventionBytesAreRemoved)
{
    // unitsInTick = 1 scrive 00 00 00 01: nel NAL diventa 00 00 03 00 01 e va letto correttamente.
    Params p;
    p.unitsInTick = 1;
    const auto nal = sps(p);
    bool escaped = false;
    for (std::size_t i = 2; i < nal.size(); ++i)
        escaped = escaped || (nal[i - 2] == 0 && nal[i - 1] == 0 && nal[i] == 3);
    ASSERT_TRUE(escaped);
    const H264Config c = parseH264Config(avcC(p));
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.sps[0].widthInMbs, 120);
}

TEST(H264Params, AnnexBIsReadToo)
{
    std::vector<std::uint8_t> annexB = {0, 0, 0, 1};
    const auto s = sps({});
    const auto q = pps({});
    annexB.insert(annexB.end(), s.begin(), s.end());
    annexB.insert(annexB.end(), {0, 0, 1});
    annexB.insert(annexB.end(), q.begin(), q.end());
    const H264Config c = parseH264Config(annexB);
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.nalLengthSize, 0);
    EXPECT_EQ(c.sps[0].widthInMbs, 120);
}

TEST(H264Params, GarbageIsNotValid)
{
    EXPECT_FALSE(parseH264Config({1, 100, 0, 40, 0xff, 0xe1}).valid);
    EXPECT_FALSE(parseH264Config({}).valid);
    EXPECT_EQ(compareMessages(parseH264Config({1, 2, 3}), parseH264Config(avcC({}))),
              (std::vector<std::string>{"parametri di codifica video diversi (non leggibili)"}));
}

TEST(H264Params, HarmlessDifferencesAreAccepted)
{
    Params level;
    level.level = 42;
    EXPECT_TRUE(differences(level).empty());

    Params timing;  // informazioni sui tempi della VUI
    timing.timeScale = 50000;
    timing.unitsInTick = 1000;
    EXPECT_TRUE(differences(timing).empty());

    Params constraints;
    constraints.constraints = 0x0c;
    EXPECT_TRUE(differences(constraints).empty());

    Params fewerRefs;  // meno fotogrammi di riferimento di quanti ne preveda il primo: va bene
    fewerRefs.refs = 2;
    EXPECT_TRUE(differences(fewerRefs).empty());
}

TEST(H264Params, RealDifferencesAreRefused)
{
    Params size;
    size.widthMbs = 80;
    size.heightMbs = 45;
    size.cropBottom = 0;
    EXPECT_EQ(differences(size), (std::vector<std::string>{"dimensioni codificate diverse"}));

    Params profile;
    profile.profile = 77;
    EXPECT_EQ(differences(profile).front(), "profilo H.264 Main invece di High");

    Params cavlc;
    cavlc.cabac = false;
    EXPECT_EQ(differences(cavlc),
              (std::vector<std::string>{"impostazioni dell'encoder diverse (CAVLC invece di CABAC)"}));

    Params frameNum;
    frameNum.frameNumBits = 6;
    EXPECT_EQ(differences(frameNum),
              (std::vector<std::string>{"impostazioni dell'encoder diverse (numerazione dei fotogrammi)"}));

    Params refs;
    refs.refs = 5;
    EXPECT_EQ(differences(refs), (std::vector<std::string>{
                                     "impostazioni dell'encoder diverse (5 fotogrammi di riferimento invece di 4)"}));

    Params qp;
    qp.initQp = 30;
    EXPECT_EQ(differences(qp),
              (std::vector<std::string>{"impostazioni dell'encoder diverse (parametri delle immagini)"}));
}

TEST(H264Params, DifferencesSayIfTheyChangeThePicture)
{
    // Solo impostazioni dell'encoder: basta ripetere i parametri giusti nel flusso.
    Params cavlc;
    cavlc.cabac = false;
    const auto settings = findH264Differences(parseH264Config(avcC(cavlc)), parseH264Config(avcC({})));
    ASSERT_EQ(settings.size(), 1u);
    EXPECT_FALSE(settings[0].changesPicture);

    Params profile;
    profile.profile = 77;
    for (const H264Difference& d : findH264Differences(parseH264Config(avcC(profile)), parseH264Config(avcC({}))))
        EXPECT_FALSE(d.changesPicture) << d.message;

    // Parametri che non si leggono: non si sa che cosa cambia, quindi si conta come immagine diversa.
    const auto unreadable = findH264Differences(parseH264Config({1, 2, 3}), parseH264Config(avcC({})));
    ASSERT_EQ(unreadable.size(), 1u);
    EXPECT_TRUE(unreadable[0].changesPicture);
}

TEST(H264Params, LevelCanBeRaised)
{
    const auto data = avcC({});
    const auto raised = withH264Level(data, 51);
    const H264Config c = parseH264Config(raised);
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.sps[0].levelIdc, 51);
    EXPECT_EQ(raised[3], 51);
    EXPECT_EQ(maxH264Level(c), 51);
    // Il resto non cambia.
    EXPECT_TRUE(compareMessages(c, parseH264Config(data)).empty());
}

MediaSignature h264File(const std::string& name, const Params& p)
{
    MediaSignature m;
    m.fileName = name;
    StreamSignature video;
    video.kind = StreamKind::Video;
    video.codec = "h264";
    video.profile = "High";
    video.width = 1920;
    video.height = 1080;
    video.pixelFormat = "yuv420p";
    video.frameRate = 59.94;
    video.extradata = avcC(p);
    m.streams = {video};
    return m;
}

TEST(H264Params, CompatibilityUsesTheRealDifferences)
{
    // Lo stesso registratore con livello e tempi diversi: si uniscono.
    Params level;
    level.level = 42;
    Params timing;
    timing.timeScale = 30000;
    const ProjectFormat format = formatOf(h264File("a.mp4", {}));
    for (const Params& p : {level, timing}) {
        const ClipFitResult same = classifyClip(h264File("x.mp4", p), format);
        EXPECT_EQ(same.fit, ClipFit::Copy);
        EXPECT_TRUE(same.reasons.empty());
    }

    // Encoder impostato diversamente: si copia ma ripetendo i suoi parametri nel flusso, con il motivo in parole
    // semplici.
    Params cavlc;
    cavlc.cabac = false;
    const ClipFitResult result = classifyClip(h264File("b.mp4", cavlc), format);
    EXPECT_EQ(result.fit, ClipFit::CopyWithParameters);
    ASSERT_EQ(result.reasons.size(), 1u);
    EXPECT_EQ(result.reasons[0], "impostazioni dell'encoder diverse (CAVLC invece di CABAC)");
}

}  // namespace
