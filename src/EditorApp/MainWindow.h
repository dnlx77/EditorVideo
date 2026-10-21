#pragma once

#include "EditorCore/EditHistory.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/SegmentExport.h"

#include <QtCore/QHash>
#include <QtGui/QImage>
#include <QtWidgets/QMainWindow>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QListWidget;
class QThread;
class QTreeWidget;

namespace editor::core {
struct Waveform;
}

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
    SequenceTimeline* sequenceTimeline() const { return timeline_; }

    // Pannelli (File del progetto, Sorgente, Montaggio, Linea del tempo del montaggio): spostabili, ridimensionabili e
    // staccabili come finestre. La disposizione si salva chiudendo il programma.
    void restoreLayout();  // quella salvata (all'avvio); se un pannello finirebbe fuori dagli schermi, torna dentro
    void resetLayout();    // Finestra > Ripristina disposizione
    // Schermo intero di un pannello (doppio clic sull'immagine di un'anteprima); di nuovo, o Esc, per uscire.
    void toggleFullScreen(QDockWidget* dock);

    // File trascinati da Esplora file (o un progetto, o una cartella): sempre aggiunti all'elenco dei file; lasciati
    // sulla Sorgente vi si apre l'ultimo, sulla linea del tempo del montaggio vanno interi nella sequenza da `index`.
    enum class DropTarget
    {
        Files,
        Source,
        Sequence,
    };
    void dropFiles(const QStringList& paths, DropTarget target, int sequenceIndex = -1);
    bool isPanelFullScreen() const { return fullScreenDock_ != nullptr; }
    QDockWidget* sourceDock() const { return sourceDock_; }
    QDockWidget* programDock() const { return programDock_; }
    QDockWidget* filesDock() const { return filesDock_; }
    QDockWidget* timelineDock() const { return timelineDock_; }
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

    // Audio della sequenza: comandi del menu Audio (usati anche dalle prove automatiche).
    void setAudioSelection(double from, double to);  // tratto (tempi della sequenza) su cui agiscono M e Maiusc+M
    void muteSelection();    // M: niente audio nel tratto selezionato
    void unmuteSelection();  // Maiusc+M: l'audio torna nel tratto selezionato
    // La traccia audio di tutta la sequenza diventa questo file. false, con un messaggio, se non si può usare.
    bool replaceAudio(const QString& path, bool showProblems = true);
    void restoreOriginalAudio();
    // Forme d'onda ancora da calcolare (0 = tutte pronte).
    int pendingWaveforms() const;

    // Progetto salvato (.evproj). interactive = false: nessuna domanda né messaggio (prove automatiche; i file
    // mancanti vengono tolti) e il progetto non va nei recenti.
    bool openProject(const QString& path, bool interactive = true);
    bool saveProject();                                              // Ctrl+S (chiede dove, la prima volta)
    bool saveProjectAs(const QString& path, bool remember = true);  // senza chiedere niente
    QString projectPath() const { return projectPath_; }
    bool isModified() const { return modified_; }

    // Annulla / Ripeti (menu Modifica): tutte le modifiche a file, montaggio e audio.
    bool undo();
    bool redo();
    QString undoText() const;  // es. "Annulla Dividi pezzo"
    // Miniature della timeline ancora da preparare, e quante sono pronte.
    int pendingThumbnails() const;
    int readyThumbnails() const;

    // Esporta il montaggio, o il solo IN-OUT se la sequenza è vuota (chiede dove salvare).
    void exportDialog();
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
    bool hasExportPlan() const;  // come !exportPlan().empty(), senza costruire il piano
    double exportLength() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;

private:
    void createMenus();
    void openFile();
    void addFilesDialog();
    void showAbout();
    void showShortcuts();
    void saveLayout();
    void exitFullScreen();
    // Un pannello staccato (o la finestra) che non sta su nessuno schermo torna su quello principale.
    void keepOnScreen(QWidget* window);

    // Incrocio fra un separatore verticale e uno orizzontale dei pannelli: trascinandolo si cambiano insieme larghezza
    // e altezza. findCorner dice quali pannelli stanno ai lati dell'incrocio in `pos` (false se lì non c'è).
    struct CornerDrag
    {
        QDockWidget* left = nullptr;
        QDockWidget* right = nullptr;
        QDockWidget* top = nullptr;
        QDockWidget* bottom = nullptr;
        QPoint start;
        int leftWidth = 0;
        int rightWidth = 0;
        int topHeight = 0;
        int bottomHeight = 0;
    };
    bool findCorner(const QPoint& pos, CornerDrag& corner) const;
    QDockWidget* dockedPanelAt(const QPoint& pos) const;
    void showInfo(const core::MediaInfo& info);
    void replaceAudioDialog();
    void openProjectDialog();
    bool saveProjectAsDialog();
    // Modifiche non salvate: chiede se salvarle. false = l'utente ha annullato.
    bool maybeSave();
    void setModified(bool modified);
    void updateTitle();
    void rememberRecent(const QString& path);
    void rebuildRecentMenu();
    core::ProjectView currentView() const;
    void resetProject();  // ferma le analisi e svuota tutto (prima di aprire un altro progetto)
    // Dopo una modifica fatta dall'utente: `before` è lo stato di prima, da ritrovare con Annulla.
    void recordEdit(const QString& label, const core::Project& before);
    void updateUndoActions();
    void afterHistoryChange(const QString& message);
    // Comandi di spostamento: vanno all'anteprima attiva.
    void stepFrame(int direction);
    // Zoom della linea del tempo dell'anteprima attiva (factor > 1 ingrandisce, 0 = Adatta).
    void zoomTimeline(double factor);
    void stepToMark(int direction);  // Sorgente: fotogramma chiave; Montaggio: inizio dei pezzi
    void seekEnd();
    void setPlaying(bool playing);
    // Miniature
    QImage thumbnailFor(int clipId, double sourceTime);
    void startNextThumbnails();
    void markAudioIn();
    void markAudioOut();
    void updateExportAction();

    // Progetto e sequenza
    void removeClip(int id, const QString& undoLabel = {});  // undoLabel vuoto: non si annulla (es. file illeggibile)
    void refreshClipList();
    void sequenceChanged();
    void refreshTimeline();
    void updateSourceBands();
    void requestWaveform(const std::string& path);
    void startNextWaveform();
    std::shared_ptr<const core::Waveform> waveformFor(const std::string& path) const;
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

    QWidget* filesPanel_ = nullptr;     // contenuto del pannello dei file
    QWidget* filesTabs_ = nullptr;      // Informazioni / Fotogrammi chiave
    QWidget* sequencePanel_ = nullptr;  // contenuto del pannello della linea del tempo
    QDockWidget* filesDock_ = nullptr;
    QDockWidget* sourceDock_ = nullptr;
    QDockWidget* programDock_ = nullptr;
    QDockWidget* timelineDock_ = nullptr;
    QDockWidget* fullScreenDock_ = nullptr;
    QByteArray layoutBeforeFullScreen_;  // disposizione da ritrovare uscendo dallo schermo intero
    std::optional<CornerDrag> cornerDrag_;  // si sta trascinando un incrocio fra separatori
    bool cornerCursor_ = false;
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
    QAction* originalAudioAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;

    core::Project project_;
    std::map<int, core::MediaInfo> clipInfo_;  // analisi dei file del progetto, per id
    int currentClipId_ = 0;                    // file mostrato nella Sorgente (0 = nessuno)
    QString currentPath_;                      // il suo percorso (nativo)
    bool updatingList_ = false;                // mentre si ricostruisce l'elenco dei file
    bool programActive_ = false;               // Spazio e frecce vanno all'anteprima del montaggio

    core::EditHistory history_;

    // Miniature della timeline: per "percorso|millisecondi" (immagine vuota = il file non ha video).
    struct ThumbnailRequest
    {
        QString key;
        std::string path;
        double seconds = 0.0;
    };
    QHash<QString, QImage> thumbnails_;
    std::deque<ThumbnailRequest> thumbnailQueue_;
    QHash<QString, bool> thumbnailPending_;
    QThread* thumbnailThread_ = nullptr;
    std::atomic<bool> cancelThumbnails_{false};

    // File di progetto
    QString projectPath_;      // vuoto = mai salvato
    bool modified_ = false;    // modifiche non salvate
    bool loading_ = false;     // si sta aprendo un progetto: le modifiche non contano
    std::optional<core::ProjectView> pendingView_;  // IN e OUT della Sorgente, da rimettere dopo l'analisi
    std::map<int, double> savedDurations_;          // durata dei file nel progetto salvato (per avvisare se cambia)

    std::deque<int> probeQueue_;  // file in attesa di analisi
    int probingClipId_ = 0;
    QThread* probeThread_ = nullptr;
    std::atomic<bool> cancelProbe_{false};

    // Forme d'onda dei file (per percorso), calcolate una alla volta in background e tenute da parte.
    std::map<std::string, std::shared_ptr<const core::Waveform>> waveforms_;
    std::deque<std::string> waveQueue_;
    std::string waveformPath_;  // in calcolo
    QThread* waveThread_ = nullptr;
    std::atomic<bool> cancelWave_{false};

    QThread* exportThread_ = nullptr;
    std::atomic<bool> cancelExport_{false};
    bool lastExportOk_ = false;
    QString lastExportProblem_;
};

// Testo con le versioni di Qt e FFmpeg, usato nella barra di stato e in "Informazioni".
QString versionSummary();

}  // namespace editor::app
