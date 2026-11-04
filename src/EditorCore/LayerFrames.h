#pragma once

#include "EditorCore/TextTracks.h"
#include "EditorCore/Transform.h"
#include "EditorCore/VideoDecoder.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace editor::core {

// I fotogrammi dei pezzi di video dei livelli sopra V1 per l'esportazione: l'esportazione avanza nel tempo, quindi ogni
// pezzo ha il suo decodificatore che va avanti insieme (senza salti, a parte quelli del montaggio). Serve il file
// originale di ogni pezzo (mai il proxy).
class LayerFrameSource
{
public:
    // `files`: il percorso (UTF-8) del file di ogni clip del progetto, per id (LayerPiece::clipId); `projectWidth` e
    // `projectHeight` danno la misura a cui decodificare (più grande se il pezzo è ingrandito).
    LayerFrameSource(std::map<int, std::string> files, int projectWidth, int projectHeight, bool allowHardware);
    ~LayerFrameSource();
    LayerFrameSource(const LayerFrameSource&) = delete;
    LayerFrameSource& operator=(const LayerFrameSource&) = delete;

    struct Visible
    {
        std::shared_ptr<VideoFrame> frame;
        Transform transform;
    };
    // I pezzi visibili al tempo `sequenceTime` della sequenza, dal livello più basso al più alto, ognuno con il suo
    // fotogramma. Un pezzo il cui file non si apre o non ha fotogrammi si salta. I decodificatori dei pezzi non più visibili
    // si chiudono.
    std::vector<Visible> at(const TextTracks& tracks, double sequenceTime);

private:
    struct Reader
    {
        std::unique_ptr<VideoDecoder> decoder;
        std::shared_ptr<VideoFrame> current;  // l'ultimo fotogramma non oltre il tempo chiesto
        std::shared_ptr<VideoFrame> next;     // quello dopo (nullptr a fine file)
        bool failed = false;
    };
    std::shared_ptr<VideoFrame> frameFor(const LayerPiece& piece, double fileTime);

    std::map<int, std::string> files_;
    int width_;
    int height_;
    bool hardware_;
    std::map<int, Reader> readers_;  // per id del pezzo
};

}  // namespace editor::core
