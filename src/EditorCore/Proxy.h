#pragma once

// Proxy: copia leggera di un video, usata solo per l'anteprima (l'esportazione usa sempre l'originale).
// - Risoluzione: quella dell'originale (con le proporzioni dei pixel applicate) divisa per 2, 4 o 8.
// - Un fotogramma chiave ogni kProxyKeyframeInterval fotogrammi e niente B-frame: un salto qualsiasi decodifica pochi
//   fotogrammi, anche quando l'originale ha i fotogrammi chiave a secondi di distanza.
// - Gli stessi fotogrammi con gli stessi tempi dell'originale: aperto con VideoDecoder dà gli stessi tempi (anche
//   l'inizio del file, scritto nei dati del proxy e letto da VideoDecoder), così IN, OUT e tagli non cambiano.
// - Solo video: l'audio si legge sempre dall'originale.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace editor::core {

inline constexpr int kProxyKeyframeInterval = 10;

// Divisori della risoluzione dell'anteprima: 1 = originale (nessun proxy).
inline constexpr int kPreviewDivisors[] = {1, 2, 4, 8};

// Nome del file del proxy (senza cartella) per l'originale e il divisore: nome dell'originale, un codice ricavato da
// percorso, dimensione e data di modifica (se l'originale cambia, il nome cambia e il proxy si rifà) e il divisore.
// Es. "vacanze_3f2a9c0d81b4e6a7_1-2.mp4". Vuoto se l'originale non esiste.
std::string proxyFileName(const std::string& originalUtf8Path, int divisor);

// Dimensioni del proxy (pari) per un video di quelle dimensioni di visualizzazione.
std::pair<int, int> proxySize(double displayWidth, double displayHeight, int divisor);

// Crea il proxy di `originalUtf8Path` in `proxyUtf8Path` (MP4, H.264 con l'encoder della scheda video se c'è). Scrive
// prima "<proxy>.part" e lo rinomina solo alla fine, così un proxy a metà non viene mai usato. `progress` riceve
// l'avanzamento (0-1); `cancel` (se c'è) interrompe. Lancia MediaError (in italiano) se non riesce o se è interrotto.
void buildProxy(const std::string& originalUtf8Path, const std::string& proxyUtf8Path, int divisor,
                const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Chiave dei dati del file proxy con l'inizio (start_time, in secondi) dell'originale: con VideoDecoder il proxy
// ha gli stessi tempi dell'originale anche se l'originale inizia con l'audio prima del video.
inline constexpr const char* kProxyStartTag = "editorvideo_start";

}  // namespace editor::core
