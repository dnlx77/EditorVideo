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

// Tutte le transizioni disponibili, nell'ordine dell'elenco.
const std::vector<const Transition*>& transitionRegistry();
// La transizione con quell'id; nullptr se non c'è (progetto fatto con una versione più nuova).
const Transition* findTransition(const std::string& id);

}  // namespace editor::core
