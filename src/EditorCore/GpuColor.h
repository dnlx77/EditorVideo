#pragma once

#include "EditorCore/Compositor.h"
#include "EditorCore/GpuDevice.h"
#include "EditorCore/TextImage.h"
#include "EditorCore/Transform.h"

#include <cstdint>
#include <memory>
#include <vector>

struct AVFrame;

namespace editor::core {

// Una scritta già disegnata (TextImage) caricata nella scheda video: si crea una volta e si disegna su tutti i fotogrammi in cui compare.
class GpuTextImage;

// Come si passa da YUV a RGB: i coefficienti della luminanza (kr, kb) e se i valori vanno da 0 a 255 invece che da 16 a 235.
struct GpuColorSpace
{
    double kr = 0.299;  // BT.601 (quello che usa oggi la composizione CPU con SWS_CS_DEFAULT)
    double kb = 0.114;
    bool fullRange = false;

    static GpuColorSpace bt601(bool full = false) { return {0.299, 0.114, full}; }
    static GpuColorSpace bt709(bool full = false) { return {0.2126, 0.0722, full}; }
};

// Il blocco base del compositore GPU: un fotogramma NV12 in memoria diventa una texture RGBA a 16 bit float (RGBA16F,
// valori 0..1, colori non premoltiplicati) e viceversa, con il ritorno in memoria. Gli shader dei livelli (G3 in poi)
// lavorano su quella texture.
// Larghezza e altezza pari (come i video). Non è thread-safe.
class GpuFrameConverter
{
public:
    // nullptr se le risorse non si riescono a creare. `device` deve vivere più a lungo del convertitore.
    static std::unique_ptr<GpuFrameConverter> create(GpuDevice& device, int width, int height);
    ~GpuFrameConverter();
    GpuFrameConverter(const GpuFrameConverter&) = delete;
    GpuFrameConverter& operator=(const GpuFrameConverter&) = delete;

    int width() const;
    int height() const;

    // Carica `nv12` (AV_PIX_FMT_NV12 in memoria, della misura del convertitore) e lo converte nella texture RGBA16F.
    bool uploadNv12(const AVFrame* nv12, const GpuColorSpace& colors);
    // Converte la texture RGBA16F in NV12 e lo scrive in `nv12` (AV_PIX_FMT_NV12 già allocato, della misura del convertitore).
    bool downloadNv12(AVFrame* nv12, const GpuColorSpace& colors);
    // Riempie la texture RGBA16F (il «fotogramma» su cui si disegnano i livelli) di un colore opaco, valori 0..1.
    bool clearCanvas(float r, float g, float b);
    // La riempie con un'immagine BGRA a 8 bit della sua stessa misura (opaca).
    bool setCanvasBgra(const BgraImage& image);
    // Disegna `source` (BGRA a 8 bit, qualunque misura) sulla texture con `transform`: stessa geometria e stesse regole di
    // compositeLayer (scala 100% = file adattato al fotogramma, ritaglio, rotazione, specchio, bordi sfumati di mezzo pixel,
    // opacità), con interpolazione bilineare della scheda e, se il pezzo si rimpicciolisce, i mipmap.
    bool drawLayer(const BgraImage& source, const Transform& transform);
    // Carica la scritta nella scheda video (nullptr se è vuota o non si riesce). Vale per questo convertitore.
    std::shared_ptr<GpuTextImage> createText(const TextImage& image);
    // Sovrappone la scritta al fotogramma come blendTextOver(transformTextImage(...)): con la trasformazione `transform` (scala,
    // rotazione, specchio, spostamento; l'opacità della trasformazione non conta) attorno al punto (`pivotX`, `pivotY`) in pixel del
    // fotogramma, e `opacity` (0..1) per le dissolvenze. Il colore è premoltiplicato come nella scritta.
    bool drawText(const GpuTextImage& text, const Transform& transform, double pivotX, double pivotY, double opacity);
    // Legge la texture RGBA16F così com'è: width * height * 4 mezzi float (valore half IEEE a 16 bit), R G B A per pixel.
    bool readRgbaHalf(std::vector<std::uint16_t>& out);

    struct Impl;

private:
    GpuFrameConverter();
    std::unique_ptr<Impl> impl_;
};

}  // namespace editor::core
