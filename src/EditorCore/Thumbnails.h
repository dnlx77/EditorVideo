#pragma once

#include "EditorCore/VideoDecoder.h"

#include <memory>
#include <string>

namespace editor::core {

// Miniature per la linea del tempo: un fotogramma piccolo di un file a un certo tempo. Usa la GPU quando c'è.
// Tiene aperto l'ultimo file usato, così più miniature dello stesso file costano poco; chiedendo il tempo di un
// fotogramma chiave si decodifica un solo fotogramma. Non è thread-safe: un thread alla volta.
class ThumbnailExtractor
{
public:
    explicit ThumbnailExtractor(int maxWidth = 160, int maxHeight = 90, bool allowHardware = true);
    ~ThumbnailExtractor();

    // Il fotogramma più vicino a `seconds`, rimpicciolito (proporzioni mantenute) nel riquadro scelto. nullptr se il
    // file non ha video o non si decodifica.
    std::shared_ptr<VideoFrame> extract(const std::string& utf8Path, double seconds);
    // Decoder usato per l'ultima miniatura: "GPU (D3D11VA)" o "software" (vuoto se nessuna).
    std::string decoderDescription() const;

private:
    int maxWidth_;
    int maxHeight_;
    bool allowHardware_;
    std::string path_;
    bool failed_ = false;  // l'ultimo file non ha video
    std::unique_ptr<VideoDecoder> decoder_;
};

}  // namespace editor::core
