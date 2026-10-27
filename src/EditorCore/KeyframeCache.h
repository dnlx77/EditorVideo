#pragma once

// Cache dei fotogrammi chiave su disco: leggere i fotogrammi chiave di un file lungo richiede di scorrere tutto il
// file; il risultato si salva in un piccolo file, così la volta dopo (ricaricando un progetto) la lettura è istantanea.
// - Il nome del file di cache dipende da percorso, dimensione e data di modifica: se il video cambia, la cache non
//   corrisponde più e si rifà. Le stesse informazioni sono ripetute dentro il file e un codice di controllo ne protegge
//   il contenuto: una cache incompleta o rovinata viene ignorata (si ricade sulla lettura normale).

#include <string>
#include <vector>

namespace editor::core {

// Nome del file di cache (senza cartella) per il video; vuoto se il video non esiste.
// Es. "vacanze_3f2a9c0d81b4e6a7.kfcache".
std::string keyframeCacheFileName(const std::string& mediaUtf8Path);

// Legge la cache di `mediaUtf8Path` dalla cartella `cacheFolder`. true solo se il file di cache esiste, è integro e
// corrisponde al video com'è ora.
bool loadKeyframeCache(const std::string& cacheFolder, const std::string& mediaUtf8Path, int& streamIndex,
                       std::vector<double>& times);

// Salva la cache (crea la cartella se manca). Scrive prima "<file>.part" e lo rinomina alla fine. Non lancia errori:
// la cache è solo un'ottimizzazione. true se salvata.
bool saveKeyframeCache(const std::string& cacheFolder, const std::string& mediaUtf8Path, int streamIndex,
                       const std::vector<double>& times);

}  // namespace editor::core
