#pragma once

#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/SegmentExport.h"

#include <QtWidgets/QMainWindow>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <vector>

class QAction;
class QLabel;
class QListWidget;
class QStackedWidget;
class QThread;
class QTreeWidget;

namespace editor::app {

class PlayerWidget;
class ProgramMonitor;
class SequenceTimeline;

// Finestra principale: a sinistra i file del progetto; al centro l'anteprima del file scelto (Sorgente, dove si
// segnano IN e OUT) e quella del montaggio; in basso la sequenza dei pezzi.
class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Nuovo progetto con questo solo file (File > Apri).
    void openPath(const QString& path);
    // Aggiunge file al progetto (File > Aggiungi file). Vengono analizzati uno alla volta in background.
    void addFiles(const QStringList& paths);

    PlayerWidget* player() const { return player_; }  // anteprima di partenza (Sorgente)
    ProgramMonitor* programMonitor() const { return program_; }
    QString currentFile() const { return currentPath_; }
    const core::Project& project() const { return project_; }
    int currentClipId() const { return currentClipId_; }

    // Sequenza: comandi del menu Montaggio (usati anche dalle prove automatiche).
    void addToSequence();        // A: IN-OUT della Sorgente (o il file intero) dopo il pezzo selezionato o in fondo
    void cutFromSequence();      // X: toglie IN-OUT della Sorgente dai pezzi di quel file
    void splitAtPlayhead();      // S: divide il pezzo sotto il cursore del montaggio
    void deleteSelection();      // Canc: il file (se il fuoco è sull'elenco dei file) o il pezzo selezionato
    void clearSequence();
    void selectClip(int id);

    // Esporta il montaggio, o il solo IN-OUT se la sequenza è vuota (chiede dove salvare).
    void exportSegment();
    // Esportazione verso `target` senza chiedere niente; showResult = false per non mostrare il messaggio finale
    // (prove automatiche). false se non si può partire (niente da esportare, analisi o esportazione in corso).
    // slowDown rallenta di proposito i passi del primo secondo e mezzo (solo per le prove: così la finestra di
    // avanzamento compare anche con file piccoli).
    bool startExport(const QString& target, bool showResult, std::chrono::milliseconds slowDown = {});
    bool isExporting() const { return exportThread_ != nullptr; }
    bool lastExportSucceeded() const { return lastExportOk_; }
    QString lastExportProblem() const { return lastExportProblem_; }  // "annullata" o il messaggio d'errore
    // Cosa verrebbe esportato adesso (vuoto = niente) e quanto dura.
    std::vector<core::ClipRanges> exportPlan() const;
    double exportLength() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createMenus();
    void openFile();
    void addFilesDialog();
    void showAbout();
    void showInfo(const core::MediaInfo& info);
    void updateExportAction();

    // Progetto e sequenza
    void removeClip(int id);
    void refreshClipList();
    void sequenceChanged();
    void updateSourceBands();
    void startNextProbe();
    int selectedClipId() const;
    void setProgramActive(bool active);

    // Comandi di riproduzione: vanno all'anteprima usata per ultima.
    void togglePlay();
    void seekRelative(double seconds);
    void seekStart();

    struct ExportOutcome
    {
        std::optional<core::ExportResult> result;
        QString error;
        bool cancelled = false;
    };
    void showExportResult(const ExportOutcome& outcome, const QString& target);

    QStackedWidget* pages_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QListWidget* clipList_ = nullptr;
    QTreeWidget* infoTree_ = nullptr;
    QLabel* keyframeTitle_ = nullptr;
    QTreeWidget* keyframeList_ = nullptr;
    QLabel* sourceTitle_ = nullptr;
    PlayerWidget* player_ = nullptr;
    ProgramMonitor* program_ = nullptr;
    SequenceTimeline* timeline_ = nullptr;
    QLabel* sequenceLabel_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* removeClipAction_ = nullptr;

    core::Project project_;
    std::map<int, core::MediaInfo> clipInfo_;  // analisi dei file del progetto, per id
    int currentClipId_ = 0;                    // file mostrato nella Sorgente (0 = nessuno)
    QString currentPath_;                      // il suo percorso (nativo)
    bool updatingList_ = false;                // mentre si ricostruisce l'elenco dei file
    bool programActive_ = false;               // Spazio e frecce vanno all'anteprima del montaggio

    std::deque<int> probeQueue_;  // file in attesa di analisi
    int probingClipId_ = 0;
    QThread* probeThread_ = nullptr;
    std::atomic<bool> cancelProbe_{false};

    QThread* exportThread_ = nullptr;
    std::atomic<bool> cancelExport_{false};
    bool lastExportOk_ = false;
    QString lastExportProblem_;
};

// Testo con le versioni di Qt e FFmpeg, usato nella barra di stato e in "Informazioni".
QString versionSummary();

}  // namespace editor::app
