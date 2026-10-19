#pragma once

#include "EditorCore/MediaError.h"
#include "EditorCore/Project.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace editor::core {

// File di progetto (.evproj): JSON leggibile in UTF-8 con i file usati, la sequenza, l'audio e lo stato delle
// anteprime. I file video e audio non vengono mai toccati: il progetto li indica soltanto, con il percorso assoluto
// e quello relativo alla cartella del progetto (così si ritrovano anche spostando la cartella intera).
//
// I tempi sono salvati come interi esatti in "flick", 1/705 600 000 di secondo: un'unità che divide esattamente le
// basi dei tempi usate da video e audio (1/90000, 1/1000, 1/48000, 1/44100, 1001/30000...), quindi un tempo
// letto da un file torna identico dopo salvataggio e riapertura.
constexpr std::int64_t kProjectTicksPerSecond = 705'600'000;
constexpr int kProjectFormatVersion = 1;
inline constexpr const char* kProjectExtension = ".evproj";

std::int64_t secondsToTicks(double seconds);
double ticksToSeconds(std::int64_t ticks);

// Stato delle anteprime da ritrovare riaprendo il progetto.
struct ProjectView
{
    int sourceClipId = 0;  // file nella Sorgente (0 = nessuno)
    std::optional<double> in;
    std::optional<double> out;
    double sourcePosition = 0.0;
    double programPosition = 0.0;

    bool operator==(const ProjectView&) const = default;
};

// Errore nel leggere o scrivere un progetto, con un messaggio in italiano.
class ProjectFileError : public MediaError
{
public:
    using MediaError::MediaError;
};

// Salva il progetto in `projectPath` (UTF-8). Il file viene scritto prima accanto e poi sostituito, così un errore a
// metà non rovina il progetto salvato prima. Lancia ProjectFileError.
void saveProjectFile(const std::string& projectPath, const Project& project, const ProjectView& view);

// Il testo JSON che verrebbe salvato (per le prove).
std::string projectToJson(const std::string& projectPath, const Project& project, const ProjectView& view);

// Un file del progetto che non si trova né al percorso assoluto né a quello relativo.
struct MissingFile
{
    int clipId = 0;            // 0 = è l'audio sostitutivo
    std::string savedPath;     // percorso assoluto salvato
    std::string fileName;      // solo il nome, per i messaggi
};

struct LoadedProject
{
    Project project;  // file non ancora analizzati: fotogrammi chiave e durata arrivano dopo, dall'analisi
    ProjectView view;
    std::vector<MissingFile> missing;
    std::map<int, double> savedDurations;  // durata dei file al momento del salvataggio (per id), se nota
};

// Legge un progetto. I percorsi vengono cercati prima così come sono stati salvati, poi relativi alla cartella del
// progetto; quelli che non si trovano finiscono in `missing` (il progetto li contiene comunque). Lancia
// ProjectFileError se il file non si apre, non è un progetto, è rovinato o è di una versione più recente.
LoadedProject loadProjectFile(const std::string& projectPath);
LoadedProject projectFromJson(const std::string& projectPath, const std::string& json);

// Il file mancante è stato ritrovato in `newPath`: lo usa al posto di quello salvato.
void relocateMissing(LoadedProject& loaded, const MissingFile& file, const std::string& newPath);
// Si prosegue senza i file mancanti: tolti dal progetto con i loro pezzi (e l'audio sostitutivo, se manca).
// Restituisce quanti pezzi della sequenza sono stati tolti.
int dropMissing(LoadedProject& loaded);

}  // namespace editor::core
