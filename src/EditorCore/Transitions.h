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
// Orologio: sfumatura del bordo della lancetta, in radianti.
constexpr double kClockFeather = 0.05;

// Tutte le transizioni disponibili, nell'ordine dell'elenco.
const std::vector<const Transition*>& transitionRegistry();
// La transizione con quell'id; nullptr se non c'è (progetto fatto con una versione più nuova).
const Transition* findTransition(const std::string& id);

}  // namespace editor::core
