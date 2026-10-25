#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace editor::core {

// Parametri di codifica H.264 letti dai "dati extra" del flusso (avcC in MP4/MKV, oppure Annex B con i codici di
// inizio). Servono a capire se due file, con parametri non identici byte per byte, si possono unire senza
// ricodifica: alcune differenze (livello, informazioni sui tempi) non contano, altre sì.

struct H264Sps
{
    int id = 0;
    int profileIdc = 0;
    int constraintFlags = 0;
    int levelIdc = 0;
    int chromaFormatIdc = 1;
    bool separateColourPlane = false;
    int bitDepthLuma = 8;
    int bitDepthChroma = 8;
    bool qpprimeYZeroTransformBypass = false;
    bool scalingMatrixPresent = false;
    int log2MaxFrameNum = 0;
    int picOrderCntType = 0;
    int log2MaxPicOrderCntLsb = 0;
    bool deltaPicOrderAlwaysZero = false;
    int offsetForNonRefPic = 0;
    int offsetForTopToBottomField = 0;
    std::vector<int> offsetForRefFrame;
    int maxNumRefFrames = 0;
    bool gapsInFrameNumAllowed = false;
    int widthInMbs = 0;
    int heightInMapUnits = 0;
    bool frameMbsOnly = true;
    bool mbAdaptiveFrameField = false;
    bool direct8x8Inference = false;
    int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    bool vuiPresent = false;
    int sarWidth = 0;   // proporzioni dei pixel (dalla VUI); 0 = non indicate
    int sarHeight = 0;
};

struct H264Pps
{
    int id = 0;
    int spsId = 0;
    bool entropyCodingCabac = false;
    bool bottomFieldPicOrderInFramePresent = false;
    int numSliceGroups = 1;
    int numRefIdxL0DefaultActive = 1;
    int numRefIdxL1DefaultActive = 1;
    bool weightedPred = false;
    int weightedBipredIdc = 0;
    int picInitQp = 26;
    int picInitQs = 26;
    int chromaQpIndexOffset = 0;
    bool deblockingFilterControlPresent = false;
    bool constrainedIntraPred = false;
    bool redundantPicCntPresent = false;
    bool transform8x8Mode = false;
    bool picScalingMatrixPresent = false;
    int secondChromaQpIndexOffset = 0;
};

struct H264Config
{
    bool valid = false;
    int nalLengthSize = 4;  // avcC: byte della lunghezza davanti a ogni NAL; 0 = Annex B
    std::vector<H264Sps> sps;
    std::vector<H264Pps> pps;
    std::vector<std::vector<std::uint8_t>> rawSps;  // NAL completi (con l'intestazione)
    std::vector<std::vector<std::uint8_t>> rawPps;
};

// Legge avcC o Annex B. valid = false se i dati non si capiscono.
H264Config parseH264Config(const std::vector<std::uint8_t>& extradata);

// Una differenza fra due insiemi di parametri.
struct H264Difference
{
    std::string message;  // in italiano, comprensibile: "profilo H.264 High invece di Main"...
    // Cambia l'immagine (formato o profondità del colore, interlacciamento, proporzioni dei pixel) o i parametri non si
    // leggono: non basta ripetere i parametri giusti nel flusso. Le altre sono solo impostazioni dell'encoder.
    bool changesPicture = false;
};

// Differenze che impediscono di decodificare i fotogrammi di `other` con i parametri di `reference`. Vuoto = si possono
// unire. Livello, vincoli e informazioni sui tempi non contano.
std::vector<H264Difference> findH264Differences(const H264Config& other, const H264Config& reference);

// Livello più alto fra quelli dei parametri (per il file unito, che deve dichiarare almeno il massimo).
int maxH264Level(const H264Config& config);

// Copia dei dati extra (avcC) con il livello portato a `level` in avcC e negli SPS. Gli altri formati restano uguali.
std::vector<std::uint8_t> withH264Level(const std::vector<std::uint8_t>& extradata, int level);

// Descrizione leggibile (per la diagnostica): profilo, livello, misure, CABAC, fotogrammi di riferimento...
std::string describeH264(const H264Config& config);

}  // namespace editor::core
