#pragma once

#include "EditorCore/Project.h"
#include "EditorCore/VideoDecoder.h"

#include <functional>
#include <memory>
#include <string>

namespace editor::core {

// Fa i fotogrammi di una transizione per l'anteprima: decodifica insieme il pezzo che finisce (con il materiale oltre la
// sua fine) e quello che inizia (con quello prima del suo inizio) e li fonde con l'effetto della transizione. Ognuno
// dei due lati tiene un decoder aperto: chiamando render() con tempi che avanzano di poco si decodifica soltanto il
// fotogramma dopo, un salto riposiziona il decoder.
// Non è thread-safe: va usato da un thread alla volta.
class TransitionRenderer
{
public:
    TransitionRenderer();
    ~TransitionRenderer();
    TransitionRenderer(const TransitionRenderer&) = delete;
    TransitionRenderer& operator=(const TransitionRenderer&) = delete;

    // Misura dei fotogrammi prodotti (i due pezzi vi stanno dentro con le loro proporzioni, bordi neri). Cambiarla
    // chiude i decoder.
    void setBox(int width, int height);
    int boxWidth() const { return boxWidth_; }
    int boxHeight() const { return boxHeight_; }
    // Da dove leggere il video di un file (il suo proxy, se c'è); nessuna funzione = dal file stesso.
    void setVideoSource(std::function<std::string(const std::string& path)> source);
    // GPU permessa o solo CPU (per le prove).
    void setHardwareAllowed(bool allowed);
    // Chiude i decoder (es. cambia il file da cui leggere).
    void reset();

    // Il fotogramma della transizione `placement` al tempo `sequenceTime` (della sequenza; fuori dal suo tratto si
    // limita agli estremi). nullptr se la transizione o i pezzi non ci sono o i file non si aprono. Il fotogramma è
    // BGRA, boxWidth x boxHeight.
    std::shared_ptr<VideoFrame> render(const Project& project, const TransitionPlacement& placement, double sequenceTime);

private:
    struct Side;
    std::unique_ptr<Side> from_;
    std::unique_ptr<Side> to_;
    int boxWidth_ = 1280;
    int boxHeight_ = 720;
    bool hardware_ = true;
    std::function<std::string(const std::string&)> videoSource_;
};

}  // namespace editor::core
