#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace editor::core {

// Direzione di uno scorrimento: da dove entra il secondo fotogramma e dove va il primo (Left = tutto va verso sinistra,
// il secondo entra da destra).
enum class TransitionDirection
{
    Left,
    Right,
    Up,
    Down,
};

// Le impostazioni di una transizione (oltre alla durata, che sta nel progetto). Ogni effetto usa solo quelle che gli
// servono.
struct TransitionParams
{
    TransitionDirection direction = TransitionDirection::Left;
};

// Un fotogramma RGBA a 8 bit non premoltiplicato, righe di `stride` byte.
struct RgbaView
{
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};
struct RgbaTarget
{
    std::uint8_t* data = nullptr;
    int stride = 0;
};

// Gli effetti dello shader delle transizioni (psTransition in GpuLayer.cpp): i numeri sono quelli che lo shader si aspetta, non
// vanno cambiati senza cambiare anche lui.
enum class GpuEffect : int
{
    Mix = 0,  // dissolvenza incrociata
    Dip = 1,  // dissolvenza al nero o al bianco
    Slide = 2,
    Wipe = 3,
    Cover = 4,
    Uncover = 5,
    Iris = 6,
    Clock = 7,
    Zoom = 8,
    Blinds = 9,
    Mosaic = 10,
    Grain = 11,
    Doors = 12,
    DiagonalWipe = 13,
    Heart = 14,
    Checkerboard = 15,
    Spiral = 16,
};

// Come disegnare una transizione sulla scheda video a un certo avanzamento: l'effetto e quattro numeri, il cui significato lo
// decide lo shader per ogni effetto (pesi dei due fotogrammi, posizione del bordo, sfumatura...). `supported` false = niente
// shader per questa transizione: si usa la CPU.
struct GpuTransition
{
    bool supported = false;
    GpuEffect effect = GpuEffect::Mix;
    float weightFrom = 0.0f;
    float weightTo = 0.0f;
    float shift = 0.0f;
    float extra = 0.0f;
};

// Una transizione: passa da un fotogramma "da" a uno "a". Aggiungerne una nuova = una classe che deriva da questa e una
// riga in transitionRegistry(): l'elenco, l'anteprima, la timeline, il salvataggio e l'esportazione la usano tramite
// l'interfaccia. Lo stesso blend serve anteprima ed esportazione, così coincidono.
class Transition
{
public:
    virtual ~Transition() = default;
    // Identificativo stabile (si salva nel progetto: non cambiarlo mai) e nome per l'elenco.
    virtual const char* id() const = 0;
    virtual const char* name() const = 0;
    // L'effetto usa la direzione?
    virtual bool usesDirection() const { return false; }
    // Scrive in `out` il risultato a `progress` (0 = solo `from`, 1 = solo `to`; fuori da 0..1 si limita). `from` e `to`
    // hanno la stessa misura; `out` può essere uno dei due. Il canale alfa è quello di `from`.
    virtual void blend(const RgbaView& from, const RgbaView& to, double progress, const TransitionParams& params,
                       RgbaTarget out) const = 0;
    // I parametri per lo shader a quell'avanzamento (fotogrammi di `width` x `height`); di serie niente GPU. Una transizione nuova
    // che li dà si disegna sulla scheda video senza toccare altro: l'elenco della GPU legge solo questo.
    virtual GpuTransition gpu(int width, int height, double progress, const TransitionParams& params) const
    {
        (void)width;
        (void)height;
        (void)progress;
        (void)params;
        return {};
    }
};

// Larghezza in pixel della sfumatura del bordo della tendina, per un quadro di `size` pixel nella direzione del movimento.
double wipeFeatherPixels(int size);

// Iris: raggio massimo (dal centro all'angolo) e larghezza in pixel della sfumatura del bordo del cerchio.
double irisMaxRadius(int width, int height);
double irisFeatherPixels(int width, int height);
// Persiana: larghezza in pixel di una striscia (8 per lato). Mosaico: lato in pixel dei blocchi a quell'avanzamento.
int blindsBandWidth(int size);
int mosaicBlockSize(double progress, int width, int height);
// Tendina diagonale: sfumatura del bordo (0..1 lungo la diagonale).
constexpr double kDiagonalFeather = 0.04;
// Dissolvenza a granuli: valore fisso in [0, 1) del pixel (x, y); il pixel passa al secondo fotogramma quando l'avanzamento lo supera.
double grainValue(int x, int y);
// Cuore: dentro la curva (x²+y²-1)³ = x²y³ (X a destra, Y in alto); scala (in pixel) con cui riempie tutto il quadro.
bool heartContains(double X, double Y);
double heartMaxScale(int width, int height);
// Scacchiera: lato in pixel dei quadrati.
int checkerCellSize(int width, int height);
// Spirale: giri e sfumatura del bordo.
constexpr double kSpiralTurns = 2.0;
constexpr double kSpiralFeather = 0.02;
// Orologio: sfumatura del bordo della lancetta, in radianti.
constexpr double kClockFeather = 0.05;

// Tutte le transizioni disponibili, nell'ordine dell'elenco.
const std::vector<const Transition*>& transitionRegistry();
// La transizione con quell'id; nullptr se non c'è (progetto fatto con una versione più nuova).
const Transition* findTransition(const std::string& id);

}  // namespace editor::core
