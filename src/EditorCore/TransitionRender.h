#pragma once

#include "EditorCore/Project.h"
#include "EditorCore/VideoDecoder.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace editor::core {

// Tutto quello che serve a fare i fotogrammi di una transizione, copiato dal progetto: così si può lavorare in un altro
// thread mentre il progetto cambia.
struct TransitionJob
{
    std::string typeId;     // della transizione (vedi Transition::id)
    TransitionParams params;
    std::string pathA;      // file del pezzo che finisce e di quello che inizia (UTF-8, non il proxy)
    std::string pathB;
    double durationA = 0.0;  // durata dei file (0 = sconosciuta)
    double durationB = 0.0;
    double fragmentStartA = 0.0;  // inizio dei pezzi nei loro file
    double fragmentStartB = 0.0;
    double sequenceStartA = 0.0;  // e nella sequenza
    double sequenceStartB = 0.0;
    double start = 0.0;  // il tratto della transizione nella sequenza
    double end = 0.0;
};

// std::nullopt se la transizione, i pezzi o i file non ci sono.
std::optional<TransitionJob> makeTransitionJob(const Project& project, const TransitionPlacement& placement);

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

    // Dove è andato il tempo nell'ultimo fotogramma (secondi), per capire cosa rallenta: decodifica dei due lati,
    // composizione e fusione; `seeks` = quante volte in tutto un decoder è stato riposizionato (invece di leggere avanti).
    struct Timing
    {
        double decodeFrom = 0.0;
        double decodeTo = 0.0;
        double blend = 0.0;
        long long seeks = 0;
        bool hardwareFrom = false;  // i due decoder usano la GPU?
        bool hardwareTo = false;
    };
    const Timing& timing() const { return timing_; }

    // Il fotogramma della transizione `placement` al tempo `sequenceTime` (della sequenza; fuori dal suo tratto si
    // limita agli estremi). nullptr se la transizione o i pezzi non ci sono o i file non si aprono. Il fotogramma è
    // BGRA, boxWidth x boxHeight.
    std::shared_ptr<VideoFrame> render(const Project& project, const TransitionPlacement& placement, double sequenceTime);
    std::shared_ptr<VideoFrame> render(const TransitionJob& job, double sequenceTime);

private:
    struct Side;
    std::unique_ptr<Side> from_;
    std::unique_ptr<Side> to_;
    Timing timing_;
    int boxWidth_ = 1280;
    int boxHeight_ = 720;
    bool hardware_ = true;
    std::function<std::string(const std::string&)> videoSource_;
};

}  // namespace editor::core
