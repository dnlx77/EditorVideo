#pragma once

#include "EditorCore/EditHistory.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/SegmentExport.h"

#include <QtCore/QCache>
#include <QtCore/QHash>
#include <QtGui/QImage>
#include <QtWidgets/QMainWindow>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QAbstractNativeEventFilter;
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
class ThumbnailCache;
class ProxyManager;
class WaveformStore;
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
    // La linea del tempo del montaggio di nuovo agganciata in basso, a tutta larghezza (menu Finestra, doppio clic sul
    // titolo quando è staccata, o lasciandola nella metà bassa della finestra).
    void redockTimeline();
    // Pannello della linea del tempo staccato e appena spostato: se il pulsante del mouse è stato rilasciato con il
    // cursore (coordinate dello schermo) nella metà bassa della finestra, si riaggancia in basso. true se l'ha fatto.
    bool redockTimelineIfDroppedLow(const QPoint& cursor, bool buttonDown);
    // Un controllo della linea del tempo staccata (ogni 50 ms, o dalle prove con valori finti): se il pulsante del mouse
    // è appena stato rilasciato dopo averla spostata, si guarda dove è stata lasciata (redockTimelineIfDroppedLow).
    void timelinePollStep(bool buttonDown, const QPoint& cursor);
    // Solo per le prove: il controllo periodico vero si spegne e i passi li dà la prova.
    void setTimelinePollAutomatic(bool automatic) { timelinePollAutomatic_ = automatic; }
    // Solo per le prove: la posizione del cursore (al posto di quella vera) e chi fa lo spostamento della finestra
    // staccata al posto di Windows (riceve la finestra e il punto in cui è stata presa).
    void setTestCursor(std::optional<QPoint> cursor) { testCursor_ = cursor; }
    void setNativeDragForTest(std::function<void(void* hwnd, QPoint grab)> drag) { nativeDragForTest_ = std::move(drag); }
    // Il titolo di un pannello agganciato evidenziato (mouse sopra: da lì si trascina).
    bool isTitleHighlighted(QDockWidget* dock) const;
    bool isDockPreviewVisible() const { return dockPreview_ != nullptr && dockPreview_->isVisible(); }
    int timelineHeight() const { return timelineHeight_; }  // altezza da agganciata, da riprendere riagganciandola

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
    // Qualità dell'anteprima (menu dei player): 1 = file originali, 2, 4, 8 = proxy a quella frazione della risoluzione.
    // Qualità dell'anteprima di un file (1 = originale, 2, 4, 8 = proxy), indipendente da quella degli altri file.
    void setPreviewQuality(const QString& path, int divisor);
    int previewQuality(const QString& path) const;
    // Cartella dei proxy: accanto al progetto (<nome>.proxy), o <cartella di lavoro>\proxy se mai salvato.
    QString proxyFolder() const;

    // Sequenza: comandi del menu Montaggio (usati anche dalle prove automatiche).
    void addToSequence();        // A: IN-OUT della Sorgente (o il file intero) dopo il pezzo selezionato o in fondo
    void cutFromSequence();      // X: toglie IN-OUT della Sorgente dai pezzi di quel file
    void splitAtPlayhead();      // S: divide il pezzo sotto il cursore del montaggio
    void deleteSelection();      // Canc: il file (se il fuoco è sull'elenco dei file) o il pezzo selezionato
    void clearSequence();
    void selectClip(int id);

    // Audio della sequenza: comandi del menu Audio (usati anche dalle prove automatiche).
    // Solo per le prove automatiche: toglie l'audio dei pezzi nel tratto (tempi della sequenza), come un passo di Annulla.
    void muteRangeForTest(double from, double to);
    // La traccia audio di tutta la sequenza diventa questo file. false, con un messaggio, se non si può usare.
    bool replaceAudio(const QString& path, bool showProblems = true);
    void restoreOriginalAudio();
    // Tracce audio (menu Audio e pulsanti "+" e "−" della linea del tempo). addAudioTrack restituisce l'id della traccia.
    int addAudioTrack();
    bool removeAudioTrack(int trackId);
    // La traccia audio dove mettere dei file nuovi: quella scelta, altrimenti la prima aggiunta, altrimenti una nuova.
    int audioTrackForNewClips();
    // Il tratto [in, out) di un file solo audio della Sorgente su una traccia audio, dal cursore del montaggio (è il
    // "Aggiungi al montaggio" dei file senza video).
    void addSourceAudioToTrack(const core::Clip& clip, double in, double out);
    // File audio (anche video, di cui si usa l'audio) sulla traccia `trackId` dal punto `seconds` della sequenza, uno
    // dopo l'altro dove c'è posto; i file entrano anche nell'elenco dei file. Restituisce quante clip ha messo.
    int addAudioFiles(const QStringList& paths, int trackId, double seconds);
    bool moveAudioClip(int clipId, int trackId, double position);
    bool trimAudioClip(int clipId, bool atStart, double value, bool ripple);
    // Scritte (step T3): livelli e scritte sopra la corsia video.
    int addTextLayer();
    bool removeTextLayer(int layerId);
    void removeSelectedTextLayer();  // quello scelto sulla linea del tempo, altrimenti l'ultimo
    void groupSelection();           // G: gli oggetti scelti con Ctrl+clic diventano un gruppo
    // Al cursore del montaggio, 5 secondi, sul livello scelto (o dove c'è posto). Con `withEditor` si apre prima l'editor
    // e la scritta si aggiunge solo se lo si conferma; 0 se non è stata aggiunta.
    int addTextClip(bool withEditor = true);
    bool editTextClip(int clipId);   // l'editor sulla scritta; con OK applica testo, stile e durata (Annulla/Ripeti)
    // Cambia testo, stile e durata della scritta (senza finestra); false e nessuna modifica se la durata non ci sta.
    bool applyTextEdit(const core::TextClip& edited);
    // La scritta trascinata sull'immagine del monitor: nuovo centro (0..1 del fotogramma).
    bool moveTextOnScreen(int clipId, double centerX, double centerY);
    bool moveTextClip(int clipId, int layerId, double position);
    bool trimTextClip(int clipId, bool atStart, double value);
    void moveFragment(int fragmentId, double start);  // sposta un pezzo (spingendo i successivi se la spinta è accesa)
    void setTrackMuted(int trackId, bool muted);
    void setTrackSolo(int trackId, bool solo);
    void setTrackLevel(int trackId, double db);
    void setEnvelope(bool fragment, int id, const std::vector<core::LevelPoint>& points);  // linea del livello di un pezzo o clip
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
    // Ricodifica all'esportazione (qualità, "Ricodifica tutto"): scelte nella finestra prima di esportare.
    // Prima di esportare il montaggio: quanti tratti si copiano e quanti si ricodificano, qualità e "Ricodifica tutto".
    // false se si rinuncia (o non si può esportare: il motivo è già stato mostrato).
    bool exportOptionsDialog();
    bool isExporting() const { return exportThread_ != nullptr; }
    bool lastExportSucceeded() const { return lastExportOk_; }
    QString lastExportProblem() const { return lastExportProblem_; }  // "annullata" o il messaggio d'errore
    // Cosa verrebbe esportato adesso (vuoto = niente) e quanto dura.
    std::vector<core::ClipRanges> exportPlan() const;
    bool hasExportPlan() const;  // come !exportPlan().empty(), senza costruire il piano
    double exportLength();  // durata del file che si esporterebbe (per le prove)
    std::vector<core::ClipRanges> previewPlan();  // come exportPlan(), con le caratteristiche dei file già lette
    double frameDurationOf(int clipId) const;  // un fotogramma del video del file (1/30 s se non si sa)
    std::optional<double> knownFrameDurationOf(int clipId) const;  // std::nullopt se non si sa (ancora) o è solo audio

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
    void relinkAudio();
    void ungroupSelection();  // U: scioglie il gruppo (o, se non è in gruppo, scollega l'audio del pezzo)
    void showGroupStatus();   // nella barra di stato: il gruppo dell'oggetto scelto
    std::vector<core::ObjectRef> selectedRefs() const;  // gli oggetti scelti, per G e U
    void addAudioFilesDialog();  // al cursore, sulla traccia scelta (o su una nuova)
    // Durata dell'audio di un file; std::nullopt (con il motivo in `problem`) se non ha audio leggibile.
    std::optional<double> audioDurationOf(const std::string& path, QString* problem = nullptr);
    void requestAudioWaveforms();  // forme d'onda di tutte le clip audio
    int colorIndexFor(const std::string& path) const;  // colore di una clip audio: quello del file, se è nel progetto
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
    // Per livello, muto e solo delle tracce audio: aggiorna la linea del tempo e il mix senza fermare la riproduzione.
    void recordMixEdit(const QString& label, const core::Project& before);
    // Come recordEdit, per le modifiche che non toccano la sequenza (nessun aggiornamento dell'interfaccia).
    void recordHistory(const QString& label, const core::Project& before);
    void updateUndoActions();
    void afterHistoryChange(const QString& message);
    // Comandi di spostamento: vanno all'anteprima attiva.
    void stepFrame(int direction);
    // Chiama `action` con l'anteprima attiva (il Montaggio o la Sorgente: hanno gli stessi comandi di spostamento).
    template <typename Action>
    void onActiveMonitor(Action&& action)
    {
        if (programActive_)
            action(*program_);
        else
            action(*player_);
    }
    // I quattro pannelli, per i cicli che li trattano tutti.
    std::array<QDockWidget*, 4> docks() const { return {filesDock_, sourceDock_, programDock_, timelineDock_}; }
    // Audio, formato e ricodifica con cui si esporta o si calcola l'anteprima dell'esportazione.
    struct ExportSettings
    {
        core::AudioEdit audio;
        core::ProjectFormat format;
        core::ReencodeOptions reencode;
    };
    ExportSettings exportSettings() const;
    // Zoom della linea del tempo dell'anteprima attiva (factor > 1 ingrandisce, 0 = Adatta).
    void zoomTimeline(double factor);
    void stepToMark(int direction);  // Sorgente: fotogramma chiave; Montaggio: inizio dei pezzi
    void seekEnd();
    void setPlaying(bool playing);
    void updateExportAction();

    // Progetto e sequenza
    void removeClip(int id, const QString& undoLabel = {});  // undoLabel vuoto: non si annulla (es. file illeggibile)
    void refreshClipList();
    void sequenceChanged();
    void refreshTimeline();
    void syncQualityMenus();  // i menu della qualità mostrano quella del file nella Sorgente e del pezzo nel Montaggio
    void addKeyframeRows();  // aggiunge il blocco successivo di fotogrammi chiave all'elenco
    void refreshWaveforms();  // solo le forme d'onda dei blocchi (quando ne finisce una)
    // Formato del progetto: se non c'è ancora e il montaggio ha dei pezzi, quello del file del primo pezzo.
    void ensureProjectFormat();
    // Caratteristiche dei flussi di un file (lette una volta e tenute da parte); nullptr se il file non si legge.
    const core::MediaSignature* signatureFor(const std::string& path);
    void projectSettingsDialog();  // Montaggio > Impostazioni del progetto
    void updateSourceBands();
    void startNextProbe();
    // Un file nuovo nel progetto: in coda per l'analisi e con la forma d'onda da calcolare.
    void analyzeNewFile(int clipId, const std::string& path);
    int selectedClipId() const;
    void setProgramActive(bool active);

    // Comandi di riproduzione: vanno all'anteprima usata per ultima.
    void togglePlay();
    void seekRelative(double seconds);
    void seekStart();
    void shuttle(int direction);  // J (-1) / L (+1): spola, vedi core::shuttleSpeed
    void stopPlayback();          // ferma e torna dove era partita la riproduzione

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
    QTimer* timelinePoll_ = nullptr;        // controlla la linea del tempo staccata (vedi timelinePollStep)
    QWidget* dockPreview_ = nullptr;        // dove andrà la linea del tempo se la si lascia adesso
    std::unique_ptr<QAbstractNativeEventFilter> titleDoubleClick_;  // doppio clic sul titolo di Windows (vedi .cpp)
    void showDockPreview(bool visible);
    bool timelinePollAutomatic_ = true;
    std::optional<QPoint> testCursor_;
    std::function<void(void* hwnd, QPoint grab)> nativeDragForTest_;
    QPoint cursorNow() const;
    // Trascinamento dal titolo della linea del tempo agganciata: lo fa Windows, non Qt (vedi startTimelineDrag).
    bool titlePressed_ = false;
    QPoint titlePressGlobal_;
    QPoint titlePressLocal_;
    void startTimelineDrag(const QPoint& global, const QPoint& local);
    bool nativeMoveLoop_ = false;
    bool movedByWindows_ = false;  // Windows sta spostando la finestra staccata (da WM_ENTERSIZEMOVE fino al rilascio)
    // Rilasciata nella metà bassa: si riaggancia quando nessun trascinamento è più in corso (né quello di Qt, che riceve
    // il rilascio dopo che il pulsante si è alzato, né lo spostamento della finestra fatto da Windows).
    void finishTimelineDrop(const QPoint& cursor, int attempts);
    bool pollButtonDown_ = false;
    QPoint pollPressPos_;                   // dov'era la linea del tempo staccata quando è stato premuto il pulsante
    void setTitleHighlight(QDockWidget* dock, bool highlighted);
    bool updateTitleHover(QDockWidget* dock, QEvent* event);  // eventi del mouse sul pannello: evidenzia il titolo
    int timelineHeight_ = 230;              // ultima altezza della linea del tempo agganciata
    bool timelineRedocking_ = false;        // riagganciata da poco: l'altezza giusta arriva dopo (vedi redockTimeline)
    void applyTimelineHeight();
    bool cornerCursor_ = false;
    QListWidget* clipList_ = nullptr;
    QTreeWidget* infoTree_ = nullptr;
    QLabel* keyframeTitle_ = nullptr;
    QTreeWidget* keyframeList_ = nullptr;
    std::vector<double> keyframeTimes_;  // quelli del file mostrato, a blocchi in keyframeList_ (vedi addKeyframeRows)
    PlayerWidget* player_ = nullptr;
    ProgramMonitor* program_ = nullptr;
    SequenceTimeline* timeline_ = nullptr;
    QLabel* sequenceLabel_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* removeClipAction_ = nullptr;
    QAction* originalAudioAction_ = nullptr;
    QAction* removeTrackAction_ = nullptr;
    QAction* removeTextLayerAction_ = nullptr;
    QAction* editTextAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;

    core::Project project_;
    std::map<int, core::MediaInfo> clipInfo_;  // analisi dei file del progetto, per id
    std::map<std::string, std::shared_ptr<const core::MediaSignature>> signatures_;  // vedi signatureFor (per percorso)
    int currentClipId_ = 0;                    // file mostrato nella Sorgente (0 = nessuno)
    QString currentPath_;                      // il suo percorso (nativo)
    bool updatingList_ = false;                // mentre si ricostruisce l'elenco dei file
    bool pushPieces_ = true;                   // Montaggio > Spingi i pezzi successivi
    bool programActive_ = false;               // Spazio e frecce vanno all'anteprima del montaggio

    core::EditHistory history_;

    // Miniature della timeline, calcolate in background (vedi ThumbnailCache).
    std::unique_ptr<ThumbnailCache> thumbnails_;

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
    std::unique_ptr<WaveformStore> waveforms_;

    // Proxy per l'anteprima (vedi ProxyManager): la qualità scelta nel menu dei player e la cartella dei proxy.
    std::unique_ptr<ProxyManager> proxies_;
    void updateProxies();         // cartella giusta e proxy mancanti chiesti (prima quello del file nella Sorgente)
    void proxyChanged(const QString& original);  // proxy pronto: le anteprime che mostrano quel file lo usano
    void deleteProxies();         // File > Elimina i proxy
    // Cartella di lavoro: dove si aprono i salvataggi di progetti ed esportazioni (vedi workFolder in .cpp).
    void chooseWorkFolder();

    QThread* exportThread_ = nullptr;
    core::ReencodeOptions reencodeOptions_;
    std::atomic<bool> cancelExport_{false};
    bool lastExportOk_ = false;
    QString lastExportProblem_;
};

// Testo con le versioni di Qt e FFmpeg, usato nella barra di stato e in "Informazioni".
QString versionSummary();

}  // namespace editor::app
