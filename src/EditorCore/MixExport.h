#pragma once

#include "EditorCore/Encoder.h"
#include "EditorCore/MixSpec.h"
#include "EditorCore/SegmentExport.h"

#include <string>

namespace editor::core {

// Mixa tutto l'audio di `spec` (dal tempo 0 alla fine della sequenza: spec.length, o la fine dell'ultimo tratto se
// è più lunga) e lo scrive in un file: di default AAC 48 kHz stereo 256 kb/s (per un M4A), che inizia al tempo 0 della
// sequenza. Con `codec` e `bitRate` (bit/s, 0 = 256 kb/s) un altro codec: l'estensione di `path` deve poterlo contenere
// (es. .mka per tutti). `path` è UTF-8. progress da 0 a 1; se restituisce false si annulla: ExportCancelled, e il file parziale si cancella. Lancia
// MediaError (in italiano) se qualcosa non va. Restituisce la durata dell'audio scritto, in secondi.
double renderMixToFile(const MixSpec& spec, const std::string& path, const ExportProgress& progress = {},
                       AudioCodec codec = AudioCodec::Aac, int bitRate = 0);

// Quanti file diversi suonano nel mix (per il messaggio dopo l'esportazione).
int mixFileCount(const MixSpec& spec);

}  // namespace editor::core
