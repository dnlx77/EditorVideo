#pragma once

#include "EditorCore/Project.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace editor::core {

// File trascinati sulla finestra (es. da Esplora file): cosa sono e cosa farne.

// Un file da aggiungere (o già nel progetto) e quanto dura.
struct DroppedMedia
{
    std::string path;     // UTF-8, assoluto
    double duration = 0.0;
    int existingClipId = 0;  // già nel progetto: non si aggiunge di nuovo (0 = nuovo)
};

struct DropPlan
{
    std::optional<std::string> project;   // un progetto (.evproj): si apre quello, il resto non conta
    std::vector<DroppedMedia> media;      // video e audio, nell'ordine in cui sono stati trascinati
    std::vector<std::string> unreadable;  // file che non si aprono o non hanno né video né audio (solo il nome)
};

// Estensioni dei file video e audio (minuscole, con il punto) che si prendono dalle cartelle trascinate.
const std::vector<std::string>& mediaExtensions();

// Dice cosa fare dei percorsi trascinati, nell'ordine:
// - un .evproj: si apre il progetto (gli altri percorsi si ignorano);
// - una cartella: i file video e audio che contiene, in ordine di nome (non le sottocartelle);
// - un file: si prova ad aprirlo con `inspect`, che restituisce la durata o std::nullopt se non è un video o un
//   audio leggibile.
// I file già nel progetto (o trascinati due volte) compaiono una volta sola, con existingClipId se c'erano già.
using MediaInspector = std::function<std::optional<double>(const std::string& utf8Path)>;
DropPlan planDrop(const std::vector<std::string>& paths, const Project& project, const MediaInspector& inspect);

// Il vero controllo per planDrop: apre il file e guarda se ha un video o un audio (veloce: non legge tutto il file).
std::optional<double> inspectMedia(const std::string& utf8Path);

// Aggiunge al progetto i file nuovi del piano e, se sequenceIndex >= 0, mette tutti i file (anche quelli che c'erano
// già) interi nella sequenza a partire da quella posizione, nell'ordine del piano. Restituisce l'id del file di ogni
// elemento di plan.media (nell'ordine) e, in `fragments`, gli id dei pezzi inseriti.
std::vector<int> applyDrop(Project& project, const std::vector<DroppedMedia>& media, int sequenceIndex,
                           std::vector<int>* fragments = nullptr);

}  // namespace editor::core
