#include "EditorApp/MainWindow.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/ProxyManager.h"
#include "EditorApp/UiText.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TimelineWidget.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/FileDrop.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/LibraryInfo.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Proxy.h"
#include "EditorCore/SegmentExport.h"
#include "EditorApp/ThumbnailCache.h"
#include "EditorApp/WaveformStore.h"
#include "EditorCore/TimeUtil.h"
#include "EditorCore/Waveform.h"

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QMimeData>
#include <QtGui/QCloseEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QCursor>
#include <QtGui/QHoverEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QStyle>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QHelpEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace editor::app {
namespace {

QString qs(const std::string& utf8)
{
    return QString::fromStdString(utf8);
}

QTreeWidgetItem* addRow(QTreeWidgetItem* parent, const QString& name, const QString& value)
{
    auto* item = new QTreeWidgetItem(parent, {name, value});
    return item;
}

QString streamTitle(const core::StreamInfo& stream)
{
    QString title = QStringLiteral("Flusso %1 - %2").arg(stream.index).arg(qs(core::streamKindName(stream.kind)));
    if (stream.attachedPicture)
        title = QStringLiteral("Flusso %1 - Copertina").arg(stream.index);
    if (!stream.language.empty())
        title += QStringLiteral(" (%1)").arg(qs(stream.language));
    return title;
}

QString codecText(const core::StreamInfo& stream)
{
    QString text = qs(stream.codec);
    if (!stream.profile.empty())
        text += QStringLiteral(", profilo %1").arg(qs(stream.profile));
    if (!stream.codecLongName.empty())
        text += QStringLiteral("  (%1)").arg(qs(stream.codecLongName));
    return text;
}

void addStream(QTreeWidget* tree, const core::StreamInfo& stream)
{
    auto* item = new QTreeWidgetItem(tree, {streamTitle(stream), qs(stream.codec)});
    addRow(item, QStringLiteral("Codec"), codecText(stream));

    switch (stream.kind) {
    case core::StreamKind::Video:
        addRow(item, QStringLiteral("Risoluzione"), QStringLiteral("%1 x %2").arg(stream.width).arg(stream.height));
        addRow(item, QStringLiteral("Frequenza fotogrammi"), qs(core::formatFrameRate(stream.frameRate)));
        addRow(item, QStringLiteral("Formato pixel"),
               stream.pixelFormat.empty() ? QStringLiteral("sconosciuto") : qs(stream.pixelFormat));
        break;
    case core::StreamKind::Audio:
        addRow(item, QStringLiteral("Frequenza di campionamento"), QStringLiteral("%1 Hz").arg(stream.sampleRate));
        addRow(item, QStringLiteral("Canali"),
               stream.channelLayout.empty() ? QString::number(stream.channels)
                                            : QStringLiteral("%1 (%2)").arg(stream.channels).arg(qs(stream.channelLayout)));
        break;
    default:
        break;
    }

    if (stream.kind == core::StreamKind::Video || stream.kind == core::StreamKind::Audio)
        addRow(item, QStringLiteral("Bitrate"), qs(core::formatBitRate(stream.bitRate)));
    if (stream.durationSeconds >= 0)
        addRow(item, QStringLiteral("Durata"), qs(core::formatDuration(stream.durationSeconds)));
    item->setExpanded(true);
}

// Risultato dell'analisi passato dal thread di lavoro alla finestra.
struct ProbeOutcome
{
    std::optional<core::MediaInfo> info;
    QString error;
    bool cancelled = false;
};

const QString kAudioFilter = QStringLiteral(
    "File audio (*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus *.wma *.ac3 *.mka);;"
    "Audio di un video (*.mp4 *.mkv *.mov *.avi *.webm);;Tutti i file (*.*)");

const QString kVideoFilter = QStringLiteral(
    "File video e audio (*.mp4 *.mkv *.mov *.avi *.m4v *.webm *.ts *.mts *.m2ts *.mpg *.wmv *.flv "
    "*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus);;Tutti i file (*.*)");

const QString kProjectExtension = QString::fromLatin1(core::kProjectExtension);  // ".evproj"
const QString kProjectFilter = QStringLiteral("Progetto EditorVideo (*%1)").arg(kProjectExtension);
constexpr int kRecentProjects = 5;
const QString kRecentProjectsKey = QStringLiteral("progettiRecenti");  // impostazione con i progetti recenti

// Titolo del pannello di un'anteprima: con il triangolo se è quella attiva (lì vanno Spazio e frecce).
QString previewTitle(const QString& name, bool active)
{
    return active ? QStringLiteral("▶ ") + name : name;
}

QSettings settings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("EditorVideo"), QStringLiteral("EditorVideo"));
}

// Cartella di lavoro: lì si aprono il salvataggio dei progetti e dell'esportazione e vanno i proxy dei progetti mai
// salvati. Di solito "Documenti\Editor video project"; si cambia da File > Cartella di lavoro. Si crea se manca.
QString workFolder()
{
    QString folder = settings().value(QStringLiteral("cartellaDiLavoro")).toString();
    if (folder.isEmpty())
        folder = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Editor video project"));
    QDir().mkpath(folder);
    return QDir::toNativeSeparators(folder);
}

// Il doppio clic sul titolo di una finestra staccata è di Windows (la barra del titolo è sua) e a Qt non arriva come
// evento che si possa intercettare: lo si prende fra i messaggi di Windows, prima che Windows lo gestisca da sé.
// EDITORVIDEO_DEBUG_DOCK=1: scrive su stderr cosa succede staccando e riagganciando la linea del tempo.
bool debugDock()
{
    static const bool enabled = qEnvironmentVariableIntValue("EDITORVIDEO_DEBUG_DOCK") != 0;
    return enabled;
}

class TitleDoubleClickFilter : public QAbstractNativeEventFilter
{
public:
    TitleDoubleClickFilter(std::function<bool(void* hwnd)> onDoubleClick, std::function<void(void* hwnd, bool moving)> onMoveLoop)
        : onDoubleClick_(std::move(onDoubleClick))
        , onMoveLoop_(std::move(onMoveLoop))
    {
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
    {
        if (eventType != "windows_generic_MSG")
            return false;
        const auto* msg = static_cast<const MSG*>(message);
        // Inizio e fine dello spostamento di una finestra da parte di Windows (trascinando il suo titolo).
        if (debugDock() && (msg->message == WM_ENTERSIZEMOVE || msg->message == WM_EXITSIZEMOVE || msg->message == WM_MOVE))
            std::fprintf(stderr, "native %x hwnd %p\n", msg->message, static_cast<void*>(msg->hwnd));
        if (msg->message == WM_ENTERSIZEMOVE || msg->message == WM_EXITSIZEMOVE) {
            onMoveLoop_(msg->hwnd, msg->message == WM_ENTERSIZEMOVE);
            return false;
        }
        if (msg->message != WM_NCLBUTTONDBLCLK || msg->wParam != HTCAPTION || !onDoubleClick_(msg->hwnd))
            return false;
        if (result != nullptr)  // per i messaggi dalla coda Qt può non passarlo
            *result = 0;
        return true;  // gestito: Windows non deve fare altro
    }

private:
    std::function<bool(void* hwnd)> onDoubleClick_;
    std::function<void(void* hwnd, bool moving)> onMoveLoop_;
};

QString nativeAbsolute(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , thumbnails_(std::make_unique<ThumbnailCache>(this, [this] { timeline_->thumbnailsChanged(); }))
    , waveforms_(std::make_unique<WaveformStore>(this, [this] { refreshWaveforms(); }))
    , proxies_(std::make_unique<ProxyManager>(this))
{
    setWindowTitle(QStringLiteral("EditorVideo"));
    resize(1500, 900);

    // --- Sinistra: file del progetto, informazioni, fotogrammi chiave
    clipList_ = new QListWidget;
    clipList_->setSelectionMode(QAbstractItemView::SingleSelection);
    clipList_->setToolTip(QStringLiteral("Clic: mostra il file nella Sorgente · Canc: toglie il file dal progetto"));
    clipList_->installEventFilter(this);
    connect(clipList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !updatingList_) {
            setProgramActive(false);
            selectClip(item->data(Qt::UserRole).toInt());
        }
    });

    infoTree_ = new QTreeWidget;
    infoTree_->setColumnCount(2);
    infoTree_->setHeaderLabels({QStringLiteral("Proprietà"), QStringLiteral("Valore")});
    infoTree_->setAlternatingRowColors(true);
    infoTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    keyframeTitle_ = new QLabel;
    keyframeTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    keyframeList_ = new QTreeWidget;
    keyframeList_->setColumnCount(2);
    keyframeList_->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Tempo")});
    keyframeList_->setRootIsDecorated(false);
    keyframeList_->setUniformRowHeights(true);
    keyframeList_->setAlternatingRowColors(true);
    keyframeList_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(keyframeList_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toBool()) {  // "Mostra altri": la riga in fondo all'elenco parziale
            addKeyframeRows();
            return;
        }
        setProgramActive(false);
        player_->seekTo(item->data(1, Qt::UserRole).toDouble());
    });
    auto* keyframePanel = new QWidget;
    auto* keyframeLayout = new QVBoxLayout(keyframePanel);
    keyframeLayout->setContentsMargins(0, 0, 0, 0);
    keyframeLayout->addWidget(keyframeTitle_);
    keyframeLayout->addWidget(keyframeList_);

    // Gli elenchi userebbero Spazio, frecce e lettere: li lasciamo ai comandi dei menu.
    infoTree_->installEventFilter(this);
    keyframeList_->installEventFilter(this);

    auto* tabs = new QTabWidget;
    tabs->setUsesScrollButtons(true);  // le schede non impongono una larghezza minima
    tabs->setElideMode(Qt::ElideRight);
    for (QWidget* list : std::initializer_list<QWidget*>{clipList_, infoTree_, keyframeList_})
        list->setMinimumSize(40, 24);
    tabs->addTab(infoTree_, QStringLiteral("Informazioni"));
    tabs->addTab(keyframePanel, QStringLiteral("Fotogrammi chiave"));

    auto* leftPanel = new QSplitter(Qt::Vertical);
    auto* clipPanel = new QWidget;
    auto* clipLayout = new QVBoxLayout(clipPanel);
    clipLayout->setContentsMargins(0, 0, 0, 0);
    clipLayout->addWidget(clipList_);
    leftPanel->addWidget(clipPanel);
    leftPanel->addWidget(tabs);
    leftPanel->setStretchFactor(0, 1);
    leftPanel->setStretchFactor(1, 2);
    // Il pannello può diventare piccolo: sotto una certa altezza resta solo l'elenco dei file (vedi eventFilter).
    leftPanel->setMinimumSize(120, 60);
    leftPanel->installEventFilter(this);
    filesPanel_ = leftPanel;
    filesTabs_ = tabs;

    // --- Centro: Sorgente (il file scelto) e Montaggio (il risultato)
    player_ = new PlayerWidget;
    player_->setIdleMessage(QStringLiteral("Apri un file video da File > Apri file video..."));
    player_->onSegmentChanged = [this] { updateExportAction(); };
    player_->onVideoDoubleClicked = [this] { toggleFullScreen(sourceDock_); };
    player_->onAddRequested = [this] { addToSequence(); };
    player_->installEventFilter(this);  // un clic sulla Sorgente la rende l'anteprima attiva
    auto* sourcePanel = new QWidget;
    auto* sourceLayout = new QVBoxLayout(sourcePanel);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(player_, 1);

    program_ = new ProgramMonitor(project_);
    program_->onActivated = [this] { setProgramActive(true); };
    program_->onPositionChanged = [this](double seconds) { timeline_->setPlayhead(seconds); };
    program_->onCacheRangeChanged = [this](double from, double to) { timeline_->setCacheRange(from, to); };
    program_->onPlaybackStarted = [this] { timeline_->resumeFollowing(); };
    program_->onVideoDoubleClicked = [this] { toggleFullScreen(programDock_); };

    // Proxy: le due anteprime leggono il video dal proxy quando è pronto; la qualità si sceglie dal menu di tutte e due.
    const auto videoSource = [this](const QString& path) { return proxies_->videoSource(path); };
    player_->videoSourceFor = videoSource;
    program_->setVideoSource(videoSource);
    player_->onPreviewQualityChosen = [this](int divisor) { setPreviewQuality(divisor); };
    program_->onPreviewQualityChosen = [this](int divisor) { setPreviewQuality(divisor); };
    proxies_->onReady = [this](const QString& original, const QString& how) {
        statusBar()->showMessage(QStringLiteral("Proxy di %1 pronto (%2)").arg(QFileInfo(original).fileName(), how), 15000);
        proxyChanged(original);
    };
    proxies_->onProgress = [this](const QString&) { refreshClipList(); };
    proxies_->onFailed = [this](const QString& original, const QString& error) {
        statusBar()->showMessage(QStringLiteral("Proxy di %1 non creato: %2").arg(QFileInfo(original).fileName(), error), 15000);
        refreshClipList();
    };
    proxies_->setFolder(proxyFolder());
    {
        int divisor = settings().value(QStringLiteral("anteprima/qualita"), 1).toInt();
        if (std::find(std::begin(core::kPreviewDivisors), std::end(core::kPreviewDivisors), divisor) == std::end(core::kPreviewDivisors))
            divisor = 1;
        proxies_->setDivisor(divisor);
        player_->setPreviewQuality(divisor);
        program_->setPreviewQuality(divisor);
    }

    // --- In basso: la sequenza
    timeline_ = new SequenceTimeline;
    timeline_->installEventFilter(this);
    timeline_->onSeek = [this](double seconds) {
        setProgramActive(true);
        program_->seekTo(seconds);
    };
    timeline_->onSelect = [this](int) { setProgramActive(true); };
    timeline_->freeStart = [this](int fragmentId, double desired) {
        return project_.sequence().nearestFreeStart(fragmentId, desired);
    };
    timeline_->onMoveTo = [this](int fragmentId, double start) {
        const core::Project before = project_;
        if (project_.sequence().moveTo(fragmentId, start))
            recordEdit(QStringLiteral("Sposta pezzo"), before);
    };
    timeline_->thumbnailFor = [this](int clipId, double sourceTime) -> QImage {
        const core::Clip* clip = project_.find(clipId);
        if (clip == nullptr || !clip->analyzed)
            return {};
        // Si usa il fotogramma chiave nello stesso punto o prima: si decodifica un solo fotogramma, e allargando la
        // finestra le miniature richieste restano in buona parte le stesse.
        return thumbnails_->get(clip->path, clip->snapToKeyframe(sourceTime));
    };
    timeline_->onFilesDropped = [this](const QStringList& paths, int index) { dropFiles(paths, DropTarget::Sequence, index); };
    timeline_->onActivate = [this](int fragmentId) {
        // Doppio clic: il pezzo torna nella Sorgente con i suoi IN e OUT.
        const core::Fragment* f = project_.sequence().find(fragmentId);
        if (f == nullptr)
            return;
        const core::Fragment fragment = *f;
        setProgramActive(false);
        selectClip(fragment.clipId);
        player_->setInOut(fragment.start, fragment.end);
        player_->seekTo(fragment.start);
    };
    timeline_->onAudioSelection = [this](double from, double to) {
        setProgramActive(true);
        if (to > from)
            statusBar()->showMessage(QStringLiteral("Tratto audio %1 - %2: M toglie l'audio, Maiusc+M lo rimette.")
                                         .arg(timeText(from), timeText(to)),
                                     8000);
    };
    timeline_->onReplacementMoved = [this](double offset) {
        const core::Project before = project_;
        project_.setAudioOffset(offset);
        recordEdit(QStringLiteral("Sposta l'audio"), before);
        statusBar()->showMessage(offset >= 0 ? QStringLiteral("L'audio sostitutivo ora parte a %1 della sequenza.").arg(timeText(offset))
                                             : QStringLiteral("L'audio sostitutivo ora parte dal suo punto %1.").arg(timeText(-offset)),
                                 8000);
    };
    sequenceLabel_ = new QLabel;
    sequenceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* sequencePanel = new QWidget;
    auto* sequenceLayout = new QVBoxLayout(sequencePanel);
    sequenceLayout->setContentsMargins(0, 0, 0, 0);
    sequenceLayout->addWidget(sequenceLabel_);
    sequenceLayout->addWidget(timeline_);
    // Anche questo può diventare basso: sotto una certa altezza la riga sopra le corsie si nasconde (vedi eventFilter).
    sequencePanel->setMinimumSize(100, 76);
    sequencePanel->installEventFilter(this);
    sequencePanel_ = sequencePanel;

    // --- Pannelli: tutto sta in pannelli spostabili; al centro non resta niente.
    const auto makeDock = [this](const QString& title, const QString& name, QWidget* content) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(name);  // serve per salvare e ripristinare la disposizione
        dock->setWidget(content);
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                          | QDockWidget::DockWidgetClosable);
        return dock;
    };
    filesDock_ = makeDock(QStringLiteral("File del progetto"), QStringLiteral("pannelloFile"), leftPanel);
    sourceDock_ = makeDock(QStringLiteral("Sorgente"), QStringLiteral("pannelloSorgente"), sourcePanel);
    programDock_ = makeDock(QStringLiteral("Montaggio"), QStringLiteral("pannelloMontaggio"), program_);
    timelineDock_ = makeDock(QStringLiteral("Linea del tempo del montaggio"), QStringLiteral("pannelloLineaDelTempo"),
                             sequencePanel);
    // Spiegazioni, nel suggerimento sul titolo del pannello (vedi eventFilter).
    filesDock_->setProperty("spiegazione", QStringLiteral("I file del progetto: clic per mostrarne uno nella Sorgente, "
                                                          "Canc per toglierlo; si aggiungono anche trascinandoli qui"));
    sourceDock_->setProperty("spiegazione", QStringLiteral("Il file scelto: segna I e O, poi A per metterlo nel montaggio"));
    programDock_->setProperty("spiegazione", QStringLiteral("Il risultato: i pezzi del montaggio uno dopo l'altro"));
    auto* center = new QWidget(this);
    center->hide();
    setCentralWidget(center);
    // File trascinati da Esplora file: ovunque nella finestra e nei pannelli, anche staccati.
    setAcceptDrops(true);
    for (QDockWidget* dock : docks()) {
        dock->setAcceptDrops(true);
        dock->installEventFilter(this);
    }
    setDockNestingEnabled(true);
    // Senza animazioni: i pannelli si sistemano subito (con il riaggancio a fine spostamento della finestra un'animazione
    // poteva restare a metà, con la linea del tempo senza spazio finché un altro pannello non si muoveva).
    setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    // La zona in basso occupa tutta la larghezza (anche sotto le zone laterali) ed è della linea del tempo, che si
    // aggancia solo lì: riagganciandola torna sempre sotto a tutto, mai sotto uno degli altri pannelli.
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    timelineDock_->setAllowedAreas(Qt::BottomDockWidgetArea);
    // Qt la riaggancia solo se la si porta a filo del bordo inferiore della finestra (la zona in basso, vuota, è
    // sottilissima): troppo difficile. Quindi: lasciata ovunque nella metà bassa della finestra si riaggancia da sola;
    // con il doppio clic sul titolo (o in qualsiasi altro modo) torna sempre in basso a tutta larghezza.
    // Doppio clic sul titolo (di Windows) della linea del tempo staccata: si riaggancia in basso.
    const auto isFloatingTimeline = [this](void* hwnd) {
        return timelineDock_->isFloating() && timelineDock_->windowHandle() != nullptr
            && reinterpret_cast<void*>(timelineDock_->windowHandle()->winId()) == hwnd;
    };
    titleDoubleClick_ = std::make_unique<TitleDoubleClickFilter>(
        [this, isFloatingTimeline](void* hwnd) {
            if (fullScreenDock_ != nullptr || !isFloatingTimeline(hwnd))
                return false;
            QTimer::singleShot(0, this, [this] { redockTimeline(); });
            return true;
        },
        [this, isFloatingTimeline](void* hwnd, bool moving) {
            if (!isFloatingTimeline(hwnd))
                return;
            // Windows comincia o finisce di spostare la linea del tempo staccata (la si sta trascinando dal titolo).
            nativeMoveLoop_ = moving;
            if (moving)
                movedByWindows_ = true;
            if (moving)
                timelinePollStep(true, cursorNow());
            else
                QTimer::singleShot(0, this, [this] { timelinePollStep(false, cursorNow()); });
        });
    qApp->installNativeEventFilter(titleDoubleClick_.get());
    // Finché è staccata la si sposta solo con lo spostamento di Windows (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE): un
    // controllo periodico aggiorna l'anteprima di dove andrà, e alla fine dello spostamento si guarda dove è stata lasciata.
    timelinePoll_ = new QTimer(this);
    timelinePoll_->setInterval(50);
    connect(timelinePoll_, &QTimer::timeout, this, [this] {
        if (timelinePollAutomatic_ && nativeMoveLoop_)
            timelinePollStep(true, cursorNow());
    });
    connect(timelineDock_, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        if (debugDock())
            std::fprintf(stderr, "topLevelChanged %d grabber %p\n", floating, static_cast<void*>(QWidget::mouseGrabber()));
        if (floating) {
            if (fullScreenDock_ == nullptr) {
                // Staccata: da qui si segue lo spostamento.
                pollButtonDown_ = nativeMoveLoop_;
                pollPressPos_ = timelineDock_->pos();
                timelinePoll_->start();
            }
            return;
        }
        timelinePoll_->stop();
        // Riagganciata da Qt (doppio clic sul titolo, trascinamento): sempre in basso, a tutta larghezza e con
        // un'altezza che si veda (Qt la rimette alta pochi pixel).
        if (fullScreenDock_ != nullptr)
            return;
        timelineRedocking_ = true;
        QTimer::singleShot(0, this, [this] {
            if (timelineDock_->isFloating()) {
                timelineRedocking_ = false;
                return;
            }
            if (dockWidgetArea(timelineDock_) != Qt::BottomDockWidgetArea || timelineDock_->width() < width() - 40)
                redockTimeline();
            else
                applyTimelineHeight();
        });
    });
    for (QDockWidget* dock : {filesDock_, sourceDock_, programDock_})
        dock->setAllowedAreas(Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    // Il titolo di un pannello agganciato si evidenzia con il mouse sopra (cursore a mano: da lì si trascina). I pannelli
    // staccati tengono il titolo di Windows, con la sua cornice (e il ridimensionamento dagli angoli).
    for (QDockWidget* dock : docks()) {
        dock->setAttribute(Qt::WA_Hover);
        dock->installEventFilter(this);
        dock->setStyleSheet(QStringLiteral("QDockWidget[titoloEvidenziato=\"true\"]::title { background: palette(highlight); }"
                                           "QDockWidget[titoloEvidenziato=\"true\"] { color: palette(highlighted-text); }"));
    }
    setMouseTracking(true);  // per il cursore sugli incroci fra separatori
    resetLayout();

    // Un clic in qualsiasi punto di un'anteprima (anche sulla sua linea del tempo) la rende quella attiva.
    for (QWidget* w : player_->findChildren<QWidget*>())
        w->installEventFilter(this);
    for (QWidget* w : program_->findChildren<QWidget*>())
        w->installEventFilter(this);

    createMenus();
    setProgramActive(false);
    sequenceChanged();
    statusBar()->showMessage(QStringLiteral("Pronto  ·  ") + versionSummary());
}

MainWindow::~MainWindow()
{
    qApp->removeNativeEventFilter(titleDoubleClick_.get());
    // Se un'analisi è ancora in corso la interrompiamo e aspettiamo che il thread finisca.
    if (probeThread_ != nullptr) {
        cancelProbe_ = true;
        probeThread_->wait();
    }
    waveforms_.reset();  // aspetta il calcolo in corso
    proxies_.reset();    // interrompe il proxy in lavoro
    thumbnails_.reset();
    // Lo stesso per un'esportazione: viene annullata e il file a metà cancellato.
    if (exportThread_ != nullptr) {
        cancelExport_ = true;
        exportThread_->wait();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Anteprima attiva

void MainWindow::setProgramActive(bool active)
{
    programActive_ = active;
    // Il titolo del pannello dice a quale anteprima vanno Spazio e frecce (▶). Cambiare il titolo cambierebbe anche la
    // voce del menu Finestra: quella resta il nome del pannello.
    sourceDock_->setWindowTitle(previewTitle(QStringLiteral("Sorgente"), !active));
    programDock_->setWindowTitle(previewTitle(QStringLiteral("Montaggio"), active));
    sourceDock_->toggleViewAction()->setText(QStringLiteral("Pannello Sorgente"));
    programDock_->toggleViewAction()->setText(QStringLiteral("Pannello Montaggio"));
}

void MainWindow::togglePlay()
{
    onActiveMonitor([](auto& monitor) { monitor.togglePlayPause(); });
}

void MainWindow::shuttle(int direction)
{
    onActiveMonitor([=](auto& monitor) { monitor.shuttle(direction); });
}

void MainWindow::stopPlayback()
{
    onActiveMonitor([](auto& monitor) { monitor.stop(); });
}

void MainWindow::seekRelative(double seconds)
{
    onActiveMonitor([=](auto& monitor) { monitor.seekRelative(seconds); });
}

void MainWindow::zoomTimeline(double factor)
{
    ZoomableTimeline* target = programActive_ ? static_cast<ZoomableTimeline*>(timeline_) : player_->timeline();
    if (factor <= 0)
        target->fitAll();
    else if (factor > 1)
        target->zoomIn();
    else
        target->zoomOut();
}

void MainWindow::stepFrame(int direction)
{
    onActiveMonitor([=](auto& monitor) { monitor.stepFrame(direction); });
}

void MainWindow::stepToMark(int direction)
{
    if (programActive_)
        program_->stepToCut(direction);
    else
        player_->stepToKeyframe(direction);
}

void MainWindow::seekEnd()
{
    onActiveMonitor([](auto& monitor) { monitor.seekToEnd(); });
}

void MainWindow::setPlaying(bool playing)
{
    if (programActive_) {
        program_->setPlaying(playing);
    } else if (player_->isPlaying() != playing) {
        player_->togglePlayPause();
    }
}

void MainWindow::seekStart()
{
    onActiveMonitor([](auto& monitor) { monitor.seekTo(0.0); });
}

// ---------------------------------------------------------------------------------------------------------------
// Sequenza

void MainWindow::addToSequence()
{
    const core::Clip* clip = project_.find(currentClipId_);
    if (clip == nullptr)
        return;
    if (!clip->analyzed) {
        player_->showNote(QStringLiteral("Attendi la fine dell'analisi del file, poi riprova."));
        return;
    }
    // IN e OUT della Sorgente; senza, il file intero.
    const core::CutSegment& segment = player_->segment();
    const double in = segment.hasIn() ? segment.in() : 0.0;
    const double out = segment.hasOut() ? segment.out() : clip->duration;

    const core::Project before = project_;
    core::Sequence& sequence = project_.sequence();
    const int selected = sequence.indexOf(timeline_->selected());
    const int gap = sequence.indexOf(timeline_->selectedGap());
    // Nello spazio vuoto selezionato (dal suo inizio), dopo il pezzo selezionato, altrimenti in fondo.
    const int at = gap >= 0 ? gap : selected >= 0 ? selected + 1 : -1;
    const int id = sequence.insert(at, clip->id, in, out);
    if (id == 0) {
        player_->showNote(QStringLiteral("Pezzo vuoto: OUT deve stare dopo IN."));
        return;
    }
    const core::Fragment* f = sequence.find(id);
    player_->clearSegment();
    player_->showNote(QStringLiteral("Aggiunto al montaggio: %1 - %2 (pezzo %3 di %4).")
                          .arg(timeText(f->start), timeText(f->end))
                          .arg(sequence.indexOf(id) + 1)
                          .arg(sequence.fragments().size()));
    timeline_->setSelected(id);
    recordEdit(QStringLiteral("Aggiungi pezzo"), before);
    program_->seekTo(sequence.startOf(sequence.indexOf(id)));
}

void MainWindow::cutFromSequence()
{
    const core::Clip* clip = project_.find(currentClipId_);
    if (clip == nullptr)
        return;
    const core::CutSegment& segment = player_->segment();
    if (!segment.hasIn() || !segment.hasOut()) {
        player_->showNote(QStringLiteral("Segna con I e O il pezzo da togliere dal montaggio, poi premi X."));
        return;
    }
    const double from = segment.in();
    const double to = segment.out();
    const core::Project before = project_;
    QString note;
    bool removed = false;
    switch (project_.sequence().removeSourceRange(clip->id, from, to)) {
    case core::Sequence::RemoveResult::Removed:
        note = QStringLiteral("Tolto %1 - %2 dal montaggio.")
                   .arg(timeText(from), timeText(to));
        player_->clearSegment();
        removed = true;
        break;
    case core::Sequence::RemoveResult::OutsideKept:
        note = QStringLiteral("Questo tratto (%1 - %2) non è nel montaggio.").arg(timeText(from), timeText(to));
        break;
    case core::Sequence::RemoveResult::Empty:
        note = QStringLiteral("Tratto vuoto: OUT deve stare dopo IN.");
        break;
    }
    player_->showNote(note);
    if (removed)
        recordEdit(QStringLiteral("Togli IN-OUT dal montaggio"), before);
}

void MainWindow::splitAtPlayhead()
{
    const double at = program_->position();
    const core::Project before = project_;
    const int id = project_.sequence().split(at);
    if (id == 0) {
        statusBar()->showMessage(QStringLiteral("Qui non si può dividere: porta il cursore dentro un pezzo, non su un "
                                                "suo bordo né in uno spazio vuoto."),
                                 8000);
        return;
    }
    const int index = project_.sequence().indexOf(id);
    statusBar()->showMessage(QStringLiteral("Pezzo diviso a %1.")
                                 .arg(timeText(project_.sequence().startOf(index))),
                             8000);
    timeline_->setSelected(id);
    recordEdit(QStringLiteral("Dividi pezzo"), before);
}

void MainWindow::deleteSelection()
{
    if (clipList_->hasFocus()) {
        if (const int id = selectedClipId(); id != 0)
            removeClip(id, QStringLiteral("Togli il file"));
        return;
    }
    const core::Project before = project_;
    if (const int gap = timeline_->selectedGap(); gap != 0) {
        // Spazio vuoto selezionato: si chiude, i pezzi dopo scorrono indietro.
        if (project_.sequence().closeGap(project_.sequence().indexOf(gap))) {
            timeline_->setSelectedGap(0);
            recordEdit(QStringLiteral("Chiudi spazio vuoto"), before);
        }
        return;
    }
    if (project_.sequence().remove(timeline_->selected())) {
        timeline_->setSelected(0);
        recordEdit(QStringLiteral("Elimina pezzo"), before);
    }
}

void MainWindow::clearSequence()
{
    if (project_.sequence().empty())
        return;
    const core::Project before = project_;
    project_.sequence().clear();
    recordEdit(QStringLiteral("Svuota il montaggio"), before);
}

// ---------------------------------------------------------------------------------------------------------------
// Annulla / Ripeti

void MainWindow::recordHistory(const QString& label, const core::Project& before)
{
    history_.record(label.toStdString(), before);
    updateUndoActions();
}

void MainWindow::recordEdit(const QString& label, const core::Project& before)
{
    recordHistory(label, before);
    sequenceChanged();
}

void MainWindow::updateUndoActions()
{
    if (undoAction_ == nullptr)
        return;
    undoAction_->setEnabled(history_.canUndo());
    undoAction_->setText(undoText());
    redoAction_->setEnabled(history_.canRedo());
    redoAction_->setText(history_.canRedo() ? QStringLiteral("&Ripeti %1").arg(qs(history_.redoLabel()))
                                            : QStringLiteral("&Ripeti"));
}

QString MainWindow::undoText() const
{
    return history_.canUndo() ? QStringLiteral("&Annulla %1").arg(qs(history_.undoLabel())) : QStringLiteral("&Annulla");
}

bool MainWindow::undo()
{
    const QString label = qs(history_.undoLabel());
    if (!history_.undo(project_))
        return false;
    afterHistoryChange(QStringLiteral("Annullato: %1").arg(label));
    return true;
}

bool MainWindow::redo()
{
    const QString label = qs(history_.redoLabel());
    if (!history_.redo(project_))
        return false;
    afterHistoryChange(QStringLiteral("Ripetuto: %1").arg(label));
    return true;
}

void MainWindow::afterHistoryChange(const QString& message)
{
    // I file tornati nel progetto senza analisi si analizzano di nuovo; quello nella Sorgente potrebbe non esserci più.
    for (const core::Clip& clip : project_.clips()) {
        waveforms_->request(clip.path);
        if (!clip.analyzed && clip.id != probingClipId_
            && std::find(probeQueue_.begin(), probeQueue_.end(), clip.id) == probeQueue_.end())
            probeQueue_.push_back(clip.id);
    }
    if (const auto& replacement = project_.audioReplacement())
        waveforms_->request(replacement->path);
    if (currentClipId_ != 0 && project_.find(currentClipId_) == nullptr) {
        currentClipId_ = 0;
        currentPath_.clear();
        player_->closeFile();
        if (!project_.empty())
            selectClip(project_.clips().front().id);
    } else if (currentClipId_ == 0 && !project_.empty()) {
        selectClip(project_.clips().front().id);
    }
    loading_ = true;
    sequenceChanged();
    loading_ = false;
    setModified(!history_.isClean());
    updateUndoActions();
    startNextProbe();
    statusBar()->showMessage(message, 5000);
}

void MainWindow::sequenceChanged()
{
    if (!loading_)
        setModified(true);
    ensureProjectFormat();
    refreshTimeline();
    program_->sequenceChanged();
    updateSourceBands();
    updateProxies();
    refreshClipList();
    updateExportAction();
}

const core::MediaSignature* MainWindow::signatureFor(const std::string& path)
{
    auto known = signatures_.find(path);
    if (known == signatures_.end()) {
        std::optional<core::MediaSignature> signature;
        try {
            signature = core::readMediaSignature(path);
        } catch (const std::exception&) {
            // File mancante o non leggibile: niente segno sul blocco (lo dice già l'elenco dei file).
        }
        known = signatures_
                    .emplace(path, signature ? std::make_shared<const core::MediaSignature>(std::move(*signature)) : nullptr)
                    .first;
    }
    return known->second.get();
}

void MainWindow::ensureProjectFormat()
{
    if (!project_.format().empty() || project_.sequence().empty())
        return;
    const core::Clip* clip = project_.find(project_.sequence().fragments().front().clipId);
    if (clip == nullptr)
        return;
    if (const core::MediaSignature* signature = signatureFor(clip->path))
        project_.setFormat(core::formatOf(*signature));
}

void MainWindow::refreshWaveforms()
{
    // Una forma d'onda finita non cambia altro nella linea del tempo: basta aggiornare quelle dei blocchi.
    const auto& replacement = project_.audioReplacement();
    timeline_->setWaveforms(
        [this](int clipId) {
            const core::Clip* clip = project_.find(clipId);
            return clip != nullptr ? waveforms_->get(clip->path) : nullptr;
        },
        replacement ? waveforms_->get(replacement->path) : nullptr);
}

void MainWindow::refreshTimeline()
{
    const core::Sequence& sequence = project_.sequence();
    std::vector<SequenceTimeline::Block> blocks;
    const bool withAudio = !project_.audioReplacement().has_value();
    for (const core::Fragment& f : sequence.fragments()) {
        const core::Clip* clip = project_.find(f.clipId);
        blocks.push_back({f.id, project_.indexOf(f.clipId), clip ? fileNameOf(clip->path) : QString(), f.length(), f.start,
                          f.clipId, clip ? waveforms_->get(clip->path) : nullptr, f.gap});
        // Come entra nel file esportato, rispetto al formato del progetto: segno sul blocco e spiegazione.
        const core::MediaSignature* signature = clip != nullptr ? signatureFor(clip->path) : nullptr;
        if (signature != nullptr && !project_.format().empty()) {
            const core::ClipFitResult fit = core::classifyClip(*signature, project_.format(), withAudio);
            SequenceTimeline::Block& block = blocks.back();
            block.fit = static_cast<int>(fit.fit);
            QString reasons;
            for (const std::string& reason : fit.reasons)
                reasons += QStringLiteral("\n• ") + qs(reason);
            if (fit.fit == core::ClipFit::CopyWithParameters)
                block.fitNote = QStringLiteral("Si copia senza ricodifica, con i suoi parametri di codifica ripetuti nel "
                                               "file esportato (diversi da quelli del formato del progetto):")
                              + reasons;
            else if (fit.fit == core::ClipFit::Reencode)
                block.fitNote = QStringLiteral("Diverso dal formato del progetto (%1): all'esportazione sarà ricodificato "
                                               "in quel formato.")
                                    .arg(qs(project_.format().sourceName))
                              + reasons;
        }
    }
    timeline_->setBlocks(std::move(blocks));
    // Zoom massimo: 12 pixel per fotogramma del primo file con un video (30 fps se non si sa ancora).
    double frame = 1.0 / 30;
    for (const core::Fragment& f : sequence.fragments()) {
        const auto info = clipInfo_.find(f.clipId);
        if (info == clipInfo_.end())
            continue;
        const auto video = std::find_if(info->second.streams.begin(), info->second.streams.end(), [](const core::StreamInfo& st) {
            return st.kind == core::StreamKind::Video && !st.attachedPicture && st.frameRate > 0;
        });
        if (video != info->second.streams.end()) {
            frame = 1.0 / video->frameRate;
            break;
        }
    }
    timeline_->setFrameDuration(frame);
    const std::vector<core::TimeRange> muted = sequence.mutedRanges();
    timeline_->setMuted(muted);
    const auto& replacement = project_.audioReplacement();
    if (replacement) {
        timeline_->setReplacement(SequenceTimeline::Replacement{fileNameOf(replacement->path), replacement->offset,
                                                                replacement->duration, waveforms_->get(replacement->path)});
    } else {
        timeline_->setReplacement(std::nullopt);
    }
    if (timeline_->hasAudioSelection() && timeline_->audioSelectionFrom() >= sequence.totalLength())
        timeline_->setAudioSelection(0.0, 0.0);
    if (originalAudioAction_ != nullptr)
        originalAudioAction_->setEnabled(replacement.has_value());

    QString text = sequence.empty() ? QStringLiteral("<b>Montaggio</b>: vuoto")
                                    : QStringLiteral("<b>Montaggio</b>: %1 pezzi  ·  durata <b>%2</b>")
                                          .arg(sequence.fragments().size())
                                          .arg(timeText(sequence.totalLength()));
    if (const int gaps = sequence.gapCount(); gaps > 0)
        text += gaps == 1 ? QStringLiteral("  ·  1 spazio vuoto") : QStringLiteral("  ·  %1 spazi vuoti").arg(gaps);
    if (replacement)
        text += QStringLiteral("  ·  audio: <b>%1</b> (al posto dell'originale)").arg(fileNameOf(replacement->path).toHtmlEscaped());
    if (!muted.empty())
        text += QStringLiteral("  ·  %1 tratti senza audio").arg(muted.size());
    sequenceLabel_->setText(text);
}

// ---------------------------------------------------------------------------------------------------------------
// Audio

void MainWindow::setAudioSelection(double from, double to)
{
    const double total = project_.sequence().totalLength();
    timeline_->setAudioSelection(std::clamp(from, 0.0, total), std::clamp(to, 0.0, total));
}

void MainWindow::markAudioIn()
{
    // Con il Montaggio attivo I e O segnano il tratto audio: da qui fino a OUT (o alla fine).
    const double at = program_->position();
    const double to = timeline_->hasAudioSelection() && timeline_->audioSelectionTo() > at + 0.001
                        ? timeline_->audioSelectionTo()
                        : project_.sequence().totalLength();
    setAudioSelection(at, to);
    statusBar()->showMessage(QStringLiteral("Tratto audio %1 - %2: segna la fine con O, poi M toglie l'audio.")
                                 .arg(timeText(at), timeText(to)),
                             8000);
}

void MainWindow::markAudioOut()
{
    const double at = program_->position();
    const double from = timeline_->hasAudioSelection() && timeline_->audioSelectionFrom() < at - 0.001
                          ? timeline_->audioSelectionFrom()
                          : 0.0;
    setAudioSelection(from, at);
    statusBar()->showMessage(QStringLiteral("Tratto audio %1 - %2: M toglie l'audio, Maiusc+M lo rimette.")
                                 .arg(timeText(from), timeText(at)),
                             8000);
}

void MainWindow::muteSelection()
{
    if (!timeline_->hasAudioSelection()) {
        statusBar()->showMessage(QStringLiteral("Seleziona un tratto trascinando sulla corsia audio (o segna I e O con il "
                                                "Montaggio attivo), poi premi M."),
                                 8000);
        return;
    }
    const double from = timeline_->audioSelectionFrom();
    const double to = timeline_->audioSelectionTo();
    const core::Project before = project_;
    if (!project_.sequence().muteRange(from, to)) {
        statusBar()->showMessage(QStringLiteral("In questo tratto l'audio è già tolto."), 5000);
        return;
    }
    timeline_->setAudioSelection(0.0, 0.0);
    statusBar()->showMessage(QStringLiteral("Audio tolto da %1 a %2: lì si sentirà silenzio.").arg(timeText(from), timeText(to)),
                             8000);
    recordEdit(QStringLiteral("Togli audio"), before);
}

void MainWindow::unmuteSelection()
{
    if (!timeline_->hasAudioSelection()) {
        statusBar()->showMessage(QStringLiteral("Seleziona sulla corsia audio il tratto in cui rimettere l'audio, poi "
                                                "premi Maiusc+M."),
                                 8000);
        return;
    }
    const double from = timeline_->audioSelectionFrom();
    const double to = timeline_->audioSelectionTo();
    const core::Project before = project_;
    if (!project_.sequence().unmuteRange(from, to)) {
        statusBar()->showMessage(QStringLiteral("In questo tratto l'audio c'è già."), 5000);
        return;
    }
    timeline_->setAudioSelection(0.0, 0.0);
    statusBar()->showMessage(QStringLiteral("Audio rimesso da %1 a %2.").arg(timeText(from), timeText(to)), 8000);
    recordEdit(QStringLiteral("Rimetti audio"), before);
}

void MainWindow::replaceAudioDialog()
{
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Audio al posto di quello originale"), QString(), kAudioFilter);
    if (!path.isEmpty())
        replaceAudio(path);
}

bool MainWindow::replaceAudio(const QString& path, bool showProblems)
{
    const std::string utf8 = nativeAbsolute(path).toStdString();
    const auto refuse = [&](const QString& problem) {
        if (showProblems)
            QMessageBox::warning(this, QStringLiteral("Audio non utilizzabile"), problem);
        statusBar()->showMessage(problem, 10000);
        return false;
    };
    // Il file esportato ha il contenitore del primo file del montaggio (o di quello nella Sorgente): l'audio deve
    // poterci stare senza ricodifica.
    std::string container;
    if (!project_.sequence().empty()) {
        if (const core::Clip* clip = project_.find(project_.sequence().fragments().front().clipId))
            container = clip->path;
    } else if (const core::Clip* clip = project_.find(currentClipId_)) {
        container = clip->path;
    }
    double duration = 0.0;
    if (!container.empty()) {
        // Il controllo apre già il file: la durata si legge da lì.
        if (const std::string problem = core::replacementAudioProblem(utf8, container, &duration); !problem.empty())
            return refuse(qs(problem));
    }
    if (container.empty()) {
        // Nessun contenitore da controllare: il file si apre qui, anche per sapere se è audio valido.
        try {
            const core::AudioDecoder decoder(utf8);
            duration = std::max(0.0, decoder.durationSeconds());
        } catch (const std::exception& e) {
            return refuse(QString::fromUtf8(e.what()));
        }
    }
    const core::Project before = project_;
    project_.setAudioReplacement(core::AudioReplacement{utf8, 0.0, duration});
    waveforms_->request(utf8);
    statusBar()->showMessage(QStringLiteral("L'audio del montaggio ora è %1 (l'audio originale è escluso). Trascina la "
                                            "sua maniglia sulla corsia audio per spostarlo.")
                                 .arg(fileNameOf(utf8)),
                             10000);
    recordEdit(QStringLiteral("Sostituisci audio"), before);
    return true;
}

void MainWindow::restoreOriginalAudio()
{
    if (!project_.audioReplacement())
        return;
    const core::Project before = project_;
    project_.setAudioReplacement(std::nullopt);
    statusBar()->showMessage(QStringLiteral("Il montaggio usa di nuovo l'audio originale dei file."), 8000);
    recordEdit(QStringLiteral("Usa l'audio originale"), before);
}

int MainWindow::pendingWaveforms() const
{
    return waveforms_->pending();
}

int MainWindow::pendingThumbnails() const
{
    return thumbnails_->pending();
}

int MainWindow::readyThumbnails() const
{
    return thumbnails_->ready();
}

void MainWindow::updateSourceBands()
{
    // Sulla linea del tempo della Sorgente, in verde i pezzi di questo file che sono nel montaggio.
    const core::Clip* clip = project_.find(currentClipId_);
    if (clip == nullptr || !clip->analyzed)
        return;
    // Ogni pezzo riportato dentro il file, poi uniti.
    std::vector<core::TimeRange> used;
    for (const core::Fragment& f : project_.sequence().fragments()) {
        if (f.clipId != clip->id)
            continue;
        const double start = core::clampTo(f.start, clip->duration);
        const double end = core::clampTo(f.end, clip->duration);
        if (end > start + core::kTimeEpsilon)
            used.push_back({start, end});
    }
    player_->setRanges(core::mergeRanges(std::move(used)));
}

// ---------------------------------------------------------------------------------------------------------------
// Esportazione

std::vector<core::ClipRanges> MainWindow::exportPlan() const
{
    if (!project_.sequence().empty())
        return project_.exportPlan(core::GapExport::Black);  // con gli spazi vuoti (neri o chiusi lo decide l'esportazione)
    // Sequenza vuota: il solo IN-OUT della Sorgente, per un taglio veloce (come allo step 4).
    const core::Clip* clip = project_.find(currentClipId_);
    const core::CutSegment& segment = player_->segment();
    if (clip != nullptr && clip->analyzed && segment.hasIn() && segment.hasOut())
        return {{clip->path, {{segment.in(), segment.out()}}}};
    return {};
}

bool MainWindow::hasExportPlan() const
{
    // Le stesse condizioni di exportPlan(): un pezzo con il suo file nel progetto, oppure (montaggio vuoto) IN e OUT
    // segnati su un file analizzato.
    const core::Sequence& sequence = project_.sequence();
    if (!sequence.empty()) {
        return std::any_of(sequence.fragments().begin(), sequence.fragments().end(),
                           [this](const core::Fragment& f) { return project_.find(f.clipId) != nullptr; });
    }
    const core::Clip* clip = project_.find(currentClipId_);
    const core::CutSegment& segment = player_->segment();
    return clip != nullptr && clip->analyzed && segment.hasIn() && segment.hasOut();
}

double MainWindow::exportLength()
{
    // Come l'esportazione: senza taglio preciso possibile IN va al fotogramma chiave prima (il file è più lungo).
    const core::ProjectFormat format = project_.sequence().empty() ? core::ProjectFormat{} : project_.format();
    const ExportSettings settings = exportSettings();
    return core::previewExport(previewPlan(), settings.audio, settings.format.empty() ? nullptr : &settings.format, settings.reencode)
        .outputSeconds;
}

MainWindow::ExportSettings MainWindow::exportSettings() const
{
    // Audio, formato e ricodifica valgono per il montaggio; il taglio veloce (sequenza vuota) copia il file com'è.
    if (project_.sequence().empty())
        return {};
    return {project_.audioEdit(core::GapExport::Black), project_.format(), reencodeOptions_};
}

std::vector<core::ClipRanges> MainWindow::previewPlan()
{
    // Per le anteprime dell'esportazione: le caratteristiche dei file si leggono una volta sola (signatureFor), non a
    // ogni calcolo. L'esportazione vera le rilegge dai file.
    std::vector<core::ClipRanges> plan = exportPlan();
    for (core::ClipRanges& clip : plan) {
        signatureFor(clip.path);
        clip.signature = signatures_.at(clip.path);
    }
    return plan;
}

void MainWindow::updateExportAction()
{
    if (exportAction_ == nullptr)
        return;
    const bool canExport = hasExportPlan();
    exportAction_->setEnabled(canExport && exportThread_ == nullptr);
    exportAction_->setText(project_.sequence().empty()
                               ? QStringLiteral("&Esporta segmento IN-OUT...")
                               : QStringLiteral("&Esporta il montaggio (%1 pezzi)...").arg(project_.sequence().fragments().size()));
    exportAction_->setToolTip(!canExport ? QStringLiteral("Metti dei pezzi nel montaggio (A) o segna IN e OUT")
                                         : QString());
    if (removeClipAction_ != nullptr)
        removeClipAction_->setEnabled(currentClipId_ != 0);
}

void MainWindow::exportDialog()
{
    const auto plan = exportPlan();
    if (plan.empty() || exportThread_ != nullptr)
        return;

    // Stesso contenitore del primo file: si propone "<nome>_montaggio.<estensione>" (o "_taglio") nella cartella di lavoro.
    const QFileInfo source(qs(plan.front().path));
    const QString extension = source.suffix();
    const QString suffix = project_.sequence().empty() ? QStringLiteral("_taglio.") : QStringLiteral("_montaggio.");
    const QString suggested = QDir(workFolder()).filePath(source.completeBaseName() + suffix + extension);
    if (!project_.sequence().empty() && !exportOptionsDialog())
        return;
    QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("Esporta"), suggested, QStringLiteral("File %1 (*.%2)").arg(extension.toUpper(), extension));
    if (target.isEmpty())
        return;
    if (QFileInfo(target).suffix().compare(extension, Qt::CaseInsensitive) != 0)
        target += QLatin1Char('.') + extension;  // il formato resta quello del progetto
    startExport(QDir::toNativeSeparators(target), true);
}

bool MainWindow::startExport(const QString& target, bool showResult, std::chrono::milliseconds slowDown)
{
    const std::vector<core::ClipRanges> plan = exportPlan();
    if (plan.empty() || exportThread_ != nullptr)
        return false;
    const ExportSettings settings = exportSettings();
    const core::AudioEdit& audio = settings.audio;
    const core::ProjectFormat& format = settings.format;
    const core::ReencodeOptions& reencode = settings.reencode;

    auto outcome = std::make_shared<ExportOutcome>();
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    const std::string outputPath = target.toStdString();

    cancelExport_ = false;
    exportThread_ = QThread::create([=, cancel = &cancelExport_] {
        const auto started = std::chrono::steady_clock::now();
        try {
            outcome->result = core::exportClips(
                plan, outputPath, audio,
                [=](double fraction) {
                    *progress = fraction;
                    // Rallentamento solo per le prove, e solo all'inizio: quanto basta perché compaia la finestra.
                    if (slowDown.count() > 0 && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500))
                        std::this_thread::sleep_for(slowDown);
                    return !cancel->load();
                },
                core::MuteMethod::Silence, format.empty() ? nullptr : &format, reencode);
        } catch (const core::ExportCancelled&) {
            outcome->cancelled = true;
        } catch (const std::exception& e) {
            outcome->error = QString::fromUtf8(e.what());
        }
    });
    exportThread_->setParent(this);

    auto* dialog = new QProgressDialog(QStringLiteral("Esportazione di %1...").arg(QFileInfo(target).fileName()),
                                       QStringLiteral("Annulla"), 0, 1000, this);
    dialog->setWindowTitle(QStringLiteral("Esporta"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(300);  // per i segmenti brevi la finestra non fa in tempo a comparire
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    connect(dialog, &QProgressDialog::canceled, this, [this] { cancelExport_ = true; });
    auto* poll = new QTimer(dialog);
    // Tempo che manca, stimato da quanto si è fatto finora (dopo qualche secondo, quando la stima è affidabile).
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    const QString label = dialog->labelText();
    connect(poll, &QTimer::timeout, dialog, [dialog, progress, elapsed, label] {
        const double done = *progress;
        dialog->setValue(static_cast<int>(done * 1000));
        const double seconds = elapsed->elapsed() / 1000.0;
        if (seconds > 3 && done > 0.02 && done < 1.0) {
            const int left = static_cast<int>(std::ceil(seconds * (1.0 - done) / done));
            dialog->setLabelText(label + QStringLiteral("\nTempo rimanente: circa %1")
                                             .arg(left < 60 ? QStringLiteral("%1 s").arg(left)
                                                            : QStringLiteral("%1 min %2 s").arg(left / 60).arg(left % 60)));
        }
    });
    poll->start(100);

    connect(exportThread_, &QThread::finished, this, [this, outcome, dialog, poll, target, showResult] {
        // Attenzione: questo codice può essere eseguito DENTRO dialog->setValue() (che, con la finestra modale,
        // elabora gli eventi in attesa), quindi la finestra non si può eliminare qui. La si spegne del tutto:
        // fermi il nostro timer e quello interno che la mostra dopo minimumDuration (reset), nascosta, ed
        // eliminata più tardi. Altrimenti restava sopra al messaggio finale, ferma al 100%.
        poll->stop();
        QObject::disconnect(dialog, nullptr, this, nullptr);  // hide/reset non devono risultare un "Annulla"
        dialog->reset();
        dialog->hide();
        dialog->deleteLater();
        exportThread_->deleteLater();
        exportThread_ = nullptr;
        updateExportAction();
        lastExportOk_ = outcome->result.has_value();
        lastExportProblem_ = outcome->cancelled ? QStringLiteral("annullata") : outcome->error;

        if (showResult) {
            // Il messaggio si mostra dal ciclo principale degli eventi, non da dentro setValue().
            QTimer::singleShot(0, this, [this, outcome, target] { showExportResult(*outcome, target); });
        }
    });

    updateExportAction();
    exportThread_->start();
    return true;
}

bool MainWindow::exportOptionsDialog()
{
    const std::vector<core::ClipRanges> plan = previewPlan();
    const core::AudioEdit audio = project_.audioEdit(core::GapExport::Black);
    const core::ProjectFormat& format = project_.format();
    // Quanti tratti si copiano e quanti si ricodificano (la prima volta prova anche gli encoder: qualche secondo).
    const auto preview = [&](bool all, bool closeGaps) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const core::ExportPreview result = core::previewExport(plan, audio, format.empty() ? nullptr : &format,
                                                               core::ReencodeOptions{reencodeOptions_.quality, all, closeGaps});
        QApplication::restoreOverrideCursor();
        return result;
    };
    const core::ExportPreview first = preview(reencodeOptions_.all, reencodeOptions_.closeGaps);
    if (!first.problem.empty()) {
        QMessageBox::warning(this, QStringLiteral("Esportazione non possibile"), qs(first.problem));
        return false;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Esporta il montaggio"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* summary = new QLabel;
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* form = new QFormLayout;
    auto* quality = new QComboBox;
    quality->addItem(QStringLiteral("Alta (quasi uguale all'originale)"));
    quality->addItem(QStringLiteral("Media (file più piccolo)"));
    quality->setCurrentIndex(reencodeOptions_.quality == core::QualityPreset::Medium ? 1 : 0);
    form->addRow(QStringLiteral("Qualità della ricodifica:"), quality);
    layout->addLayout(form);
    auto* all = new QCheckBox(QStringLiteral("Ricodifica tutto (massima compatibilità con i lettori, più lento)"));
    all->setChecked(reencodeOptions_.all);
    layout->addWidget(all);
    // Spazi vuoti: di solito diventano nero e silenzio; si possono anche chiudere (i pezzi uno dopo l'altro).
    auto* closeGaps = new QCheckBox(QStringLiteral("Chiudi gli spazi vuoti (i pezzi uno dopo l'altro, senza nero)"));
    closeGaps->setChecked(reencodeOptions_.closeGaps);
    closeGaps->setVisible(project_.sequence().gapCount() > 0);
    layout->addWidget(closeGaps);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Esporta..."));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto describe = [&](const core::ExportPreview& p) {
        QString text = QStringLiteral("<b>%1 pezzi copiati</b> così come sono (%2)").arg(p.copiedSegments).arg(timeText(p.copiedSeconds));
        if (p.reencodedSegments > 0)
            text += QStringLiteral(", <b>%1 ricodificati</b> nel formato del progetto (%2)")
                        .arg(p.reencodedSegments)
                        .arg(timeText(p.reencodedSeconds));
        if (p.preciseCuts > 0)
            text += QStringLiteral(", <b>%1 tagli precisi</b> (%2 ricodificati ai bordi, fra il taglio e il fotogramma chiave più vicino)")
                        .arg(p.preciseCuts)
                        .arg(timeText(p.preciseSeconds));
        if (p.blackGaps > 0)
            text += p.blackGaps == 1 ? QStringLiteral(", <b>1 spazio nero</b> (%1)").arg(timeText(p.blackSeconds))
                                     : QStringLiteral(", <b>%1 spazi neri</b> (%2)").arg(p.blackGaps).arg(timeText(p.blackSeconds));
        text += QStringLiteral(".<br>Durata del file: <b>%1</b>.").arg(timeText(p.outputSeconds));
        if (!p.closedGaps.empty())
            text += QStringLiteral("<br><span style='color:#d08a1a'>%1</span>").arg(qs(p.closedGaps).toHtmlEscaped());
        if (!p.impreciseCuts.empty())
            text += QStringLiteral("<br><span style='color:#d08a1a'>%1</span>").arg(qs(p.impreciseCuts).toHtmlEscaped());
        if (p.reencodedSegments > 0 && p.copiedSegments > 0)
            text += QStringLiteral("<br>I pezzi copiati non perdono niente; quelli ricodificati sono quasi uguali "
                                   "all'originale con la qualità alta.");
        summary->setText(text);
        quality->setEnabled(p.reencodedSegments > 0 || p.preciseCuts > 0 || p.blackGaps > 0);
    };
    describe(first);
    const auto update = [&] { describe(preview(all->isChecked(), closeGaps->isChecked())); };
    connect(all, &QCheckBox::toggled, &dialog, update);
    connect(closeGaps, &QCheckBox::toggled, &dialog, update);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    reencodeOptions_.quality = quality->currentIndex() == 1 ? core::QualityPreset::Medium : core::QualityPreset::High;
    reencodeOptions_.all = all->isChecked();
    reencodeOptions_.closeGaps = closeGaps->isChecked();
    return true;
}

void MainWindow::showExportResult(const ExportOutcome& outcome, const QString& target)
{
    if (outcome.cancelled) {
        statusBar()->showMessage(QStringLiteral("Esportazione annullata"), 5000);
        return;
    }
    if (!outcome.result) {
        QMessageBox::warning(this, QStringLiteral("Esportazione non riuscita"), outcome.error);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Esportato in %1").arg(target), 10000);
    const int count = outcome.result->segmentCount;
    const QString what = count > 1 ? QStringLiteral("Montaggio di %1 pezzi salvato").arg(count)
                                   : QStringLiteral("Segmento salvato");
    QMessageBox::information(
        this, QStringLiteral("Esportazione completata"),
        QStringLiteral("%1 in:\n%2\n\nDurata: %3\n%4")
            .arg(what, target, timeText(outcome.result->durationSeconds),
                 outcome.result->reencodedSegments == 0
                     ? QStringLiteral("Senza ricodifica: stessa qualità dell'originale.")
                     : QStringLiteral("%1 tratti ricodificati, gli altri copiati senza ricodifica.").arg(outcome.result->reencodedSegments)));
}

// ---------------------------------------------------------------------------------------------------------------
// Tasti

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Linea del tempo staccata: il titolo di Windows non va a Qt. Premendo lì Qt avvierebbe il suo trascinamento dei
    // pannelli, che apre nella finestra uno spazio di anteprima; il rilascio però se lo prende lo spostamento di Windows,
    // quindi quel trascinamento non finisce mai e la disposizione resta bloccata (Sorgente e Montaggio alti fino in fondo,
    // nemmeno "Ripristina disposizione" funziona) finché un altro pannello non si muove. Lo spostamento lo fa comunque
    // Windows, e il riaggancio lo gestisce questa finestra (vedi timelinePollStep).
    if (watched == timelineDock_ && timelineDock_->isFloating()
        && (event->type() == QEvent::NonClientAreaMouseButtonPress || event->type() == QEvent::NonClientAreaMouseMove
            || event->type() == QEvent::NonClientAreaMouseButtonRelease))
        return true;
    // Suggerimento sul titolo di un pannello agganciato (solo sul titolo: dentro valgono quelli del contenuto).
    if (auto* dock = qobject_cast<QDockWidget*>(watched); dock != nullptr && event->type() == QEvent::ToolTip
        && !dock->isFloating() && dock->widget() != nullptr) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = dock->property("spiegazione").toString();
        if (help->pos().y() < dock->widget()->geometry().top() && !text.isEmpty()) {
            QToolTip::showText(help->globalPos(), text, dock);
            return true;
        }
    }
    // Linea del tempo staccata: il doppio clic sul titolo la riaggancia sempre. Il titolo di una finestra staccata è
    // quello di Windows, che col doppio clic la ingrandirebbe: lo si intercetta prima.
    if (watched == timelineDock_ && timelineDock_->isFloating() && fullScreenDock_ == nullptr
        && (event->type() == QEvent::NonClientAreaMouseButtonDblClick
            || (event->type() == QEvent::MouseButtonDblClick && timelineDock_->widget() != nullptr
                && static_cast<QMouseEvent*>(event)->position().y() < timelineDock_->widget()->geometry().top()))) {
        QTimer::singleShot(0, this, [this] { redockTimeline(); });
        return true;
    }
    // Linea del tempo agganciata trascinata dal titolo: non la stacca Qt (il suo trascinamento lascia la zona in basso in
    // uno stato da cui il riaggancio non funziona) ma la si stacca qui e la si fa spostare a Windows, come quando è già
    // staccata.
    if (watched == timelineDock_ && !timelineDock_->isFloating() && fullScreenDock_ == nullptr) {
        const auto inTitle = [this](const QPointF& pos) {
            return timelineDock_->widget() != nullptr && pos.y() >= 0 && pos.y() < timelineDock_->widget()->geometry().top();
        };
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && inTitle(mouse->position())) {
                titlePressed_ = true;
                titlePressGlobal_ = mouse->globalPosition().toPoint();
                titlePressLocal_ = mouse->position().toPoint();
                timelineDock_->setCursor(Qt::ClosedHandCursor);
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            if (!titlePressed_)
                break;
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if ((mouse->globalPosition().toPoint() - titlePressGlobal_).manhattanLength() >= QApplication::startDragDistance()) {
                titlePressed_ = false;
                startTimelineDrag(mouse->globalPosition().toPoint(), titlePressLocal_);
            }
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (titlePressed_) {
                titlePressed_ = false;
                timelineDock_->setCursor(Qt::OpenHandCursor);
                return true;
            }
            break;
        case QEvent::MouseButtonDblClick:
            if (inTitle(static_cast<QMouseEvent*>(event)->position()))
                return true;  // agganciata resta agganciata
            break;
        default:
            break;
        }
    }
    // Titolo dei pannelli agganciati evidenziato con il mouse sopra.
    if (auto* hovered = qobject_cast<QDockWidget*>(watched); hovered != nullptr && updateTitleHover(hovered, event))
        return false;
    // L'altezza della linea del tempo agganciata, da riprendere quando la si riaggancia dopo averla staccata.
    if (watched == timelineDock_ && event->type() == QEvent::Resize && !timelineDock_->isFloating()
        && !timelineRedocking_ && fullScreenDock_ == nullptr && timelineDock_->isVisible())
        timelineHeight_ = timelineDock_->height();

    // Pannelli piccoli: si nasconde quello che non ci sta (le schede sotto i file, la riga sopra le corsie).
    if (event->type() == QEvent::Resize) {
        if (watched == filesPanel_)
            filesTabs_->setVisible(filesPanel_->height() >= 170);
        else if (watched == sequencePanel_)
            sequenceLabel_->setVisible(sequencePanel_->height() >= 110);
    }
    // File trascinati su un pannello (la linea del tempo del montaggio li gestisce da sé).
    if (auto* dock = qobject_cast<QDockWidget*>(watched)) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* drag = static_cast<QDragMoveEvent*>(event);
            if (!droppedFiles(drag->mimeData()).isEmpty()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            const QStringList files = droppedFiles(drop->mimeData());
            if (!files.isEmpty()) {
                drop->acceptProposedAction();
                // Dopo, fuori dal trascinamento (che in Windows tiene fermo Esplora file finché non finisce).
                const DropTarget target = dock == sourceDock_ ? DropTarget::Source : DropTarget::Files;
                QTimer::singleShot(0, this, [this, files, target] { dropFiles(files, target); });
                return true;
            }
        }
    }

    // Un clic su un'anteprima la rende quella attiva (Spazio e frecce vanno lì).
    if (event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget != nullptr && (widget == player_ || player_->isAncestorOf(widget)))
            setProgramActive(false);
        else if (widget != nullptr && program_->isAncestorOf(widget))
            setProgramActive(true);
    }

    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        static const int commandKeys[] = {Qt::Key_Space, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End,
                                          Qt::Key_Comma, Qt::Key_Period, Qt::Key_I, Qt::Key_O, Qt::Key_A, Qt::Key_X,
                                          Qt::Key_S, Qt::Key_M, Qt::Key_J, Qt::Key_K, Qt::Key_L, Qt::Key_Z,
                                          Qt::Key_Y, Qt::Key_Plus, Qt::Key_Minus, Qt::Key_Equal, Qt::Key_Delete};
        const bool commandKey = std::find(std::begin(commandKeys), std::end(commandKeys), key->key())
                             != std::end(commandKeys);
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        // Bloccando l'evento senza accettarlo, il tasto va alla scorciatoia del menu (Canc compreso: il menu
        // Montaggio sa se togliere il file o il pezzo).
        if (commandKey && (modifiers == Qt::NoModifier || modifiers == Qt::ControlModifier
                           || modifiers == Qt::ShiftModifier || modifiers == (Qt::ControlModifier | Qt::ShiftModifier)))
            return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!droppedFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QStringList files = droppedFiles(event->mimeData());
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    QTimer::singleShot(0, this, [this, files] { dropFiles(files, DropTarget::Files); });
}

void MainWindow::dropFiles(const QStringList& paths, DropTarget target, int sequenceIndex)
{
    std::vector<std::string> utf8;
    for (const QString& path : paths)
        utf8.push_back(nativeAbsolute(path).toStdString());
    const core::DropPlan plan = core::planDrop(utf8, project_, core::inspectMedia);

    // Un progetto si apre (chiedendo se salvare quello di adesso).
    if (plan.project) {
        if (maybeSave())
            openProject(qs(*plan.project));
        return;
    }
    if (!plan.unreadable.empty()) {
        QString list;
        for (const std::string& name : plan.unreadable)
            list += QStringLiteral("\n• %1").arg(qs(name));
        QMessageBox::warning(this, QStringLiteral("File non aggiunti"),
                             QStringLiteral("Questi file non sono video o audio leggibili e non sono stati aggiunti:%1%2")
                                 .arg(list, plan.media.empty() ? QString() : QStringLiteral("\n\nGli altri sì.")));
    }
    if (plan.media.empty())
        return;

    // Una sola modifica, da annullare tutta insieme: file nuovi nell'elenco (senza doppioni) e, sulla linea del tempo,
    // tutti i file interi nella sequenza nel punto di rilascio.
    const core::Project before = project_;
    std::vector<int> fragments;
    const std::vector<int> ids =
        core::applyDrop(project_, plan.media, target == DropTarget::Sequence ? std::max(0, sequenceIndex) : -1, &fragments);
    int added = 0;
    for (std::size_t i = 0; i < plan.media.size(); ++i) {
        if (plan.media[i].existingClipId != 0)
            continue;
        ++added;
        probeQueue_.push_back(ids[i]);
        waveforms_->request(plan.media[i].path);
    }
    if (added > 0 || !fragments.empty())
        recordEdit(QStringLiteral("Aggiungi file"), before);
    if (target == DropTarget::Source || currentClipId_ == 0) {
        setProgramActive(false);
        selectClip(ids.back());  // l'ultimo file trascinato
    }
    if (!fragments.empty()) {
        timeline_->setSelected(fragments.front());
        setProgramActive(true);
        program_->seekTo(project_.sequence().startOf(project_.sequence().indexOf(fragments.front())));
    }
    refreshClipList();
    startNextProbe();
    const int already = static_cast<int>(plan.media.size()) - added;
    QString message = added > 0 ? QStringLiteral("Aggiunti %1 file al progetto").arg(added)
                                : QStringLiteral("I file erano già nel progetto");
    if (added > 0 && already > 0)
        message += QStringLiteral(" (%1 c'erano già)").arg(already);
    if (!fragments.empty())
        message += QStringLiteral(", %1 pezzi messi nel montaggio").arg(fragments.size());
    statusBar()->showMessage(message + QStringLiteral("."), 8000);
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    openAction_ = fileMenu->addAction(QStringLiteral("&Apri file video..."), this, &MainWindow::openFile);
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setStatusTip(QStringLiteral("Apre un file in un nuovo progetto"));
    QAction* openProjectAction = fileMenu->addAction(QStringLiteral("Apri &progetto..."), this, &MainWindow::openProjectDialog);
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));  // Ctrl+O apre già un file video
    recentMenu_ = fileMenu->addMenu(QStringLiteral("Progetti &recenti"));
    connect(recentMenu_, &QMenu::aboutToShow, this, [this] { rebuildRecentMenu(); });
    QAction* saveAction = fileMenu->addAction(QStringLiteral("&Salva progetto"), this, [this] { saveProject(); });
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setStatusTip(QStringLiteral("Salva file, montaggio e audio in un file %1 (gli originali non vengono toccati)")
                                 .arg(kProjectExtension));
    QAction* saveAsAction = fileMenu->addAction(QStringLiteral("Salva progetto &con nome..."), this, [this] { saveProjectAsDialog(); });
    saveAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    fileMenu->addSeparator();
    QAction* addAction = fileMenu->addAction(QStringLiteral("A&ggiungi file..."), this, &MainWindow::addFilesDialog);
    addAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    addAction->setStatusTip(QStringLiteral("Aggiunge altri file al progetto"));
    removeClipAction_ = fileMenu->addAction(QStringLiteral("&Togli il file dal progetto"), this, [this] {
        if (currentClipId_ != 0)
            removeClip(currentClipId_, QStringLiteral("Togli il file"));
    });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Cartella di &lavoro..."), this, &MainWindow::chooseWorkFolder)
        ->setStatusTip(QStringLiteral("Dove si aprono il salvataggio dei progetti e dell'esportazione"));
    fileMenu->addAction(QStringLiteral("Elimina i pro&xy del progetto"), this, &MainWindow::deleteProxies)
        ->setStatusTip(QStringLiteral("Cancella le copie leggere per l'anteprima (si ricreano se servono)"));
    fileMenu->addSeparator();
    exportAction_ = fileMenu->addAction(QStringLiteral("&Esporta..."), this, &MainWindow::exportDialog);
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    exportAction_->setStatusTip(QStringLiteral("Salva il montaggio in un nuovo file, ricodificando solo dove serve"));
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QStringLiteral("&Esci"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));

    QMenu* editMenuUndo = menuBar()->addMenu(QStringLiteral("M&odifica"));
    undoAction_ = editMenuUndo->addAction(QStringLiteral("&Annulla"), this, [this] { undo(); });
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = editMenuUndo->addAction(QStringLiteral("&Ripeti"), this, [this] { redo(); });
    redoAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});

    // I comandi di riproduzione e spostamento vanno all'anteprima attiva (quella usata per ultima).
    QMenu* playMenu = menuBar()->addMenu(QStringLiteral("&Riproduzione"));
    QAction* playAction = playMenu->addAction(QStringLiteral("Riproduci / &Pausa (anteprima attiva)"), this,
                                              [this] { togglePlay(); });
    playAction->setShortcut(QKeySequence(Qt::Key_Space));
    // Spola come nei programmi professionali: L avanti, J indietro (premuti più volte vanno più veloci), K ferma.
    playMenu->addAction(QStringLiteral("Avanti (più volte: più veloce)"), this, [this] { shuttle(+1); })
        ->setShortcut(QKeySequence(Qt::Key_L));
    playMenu->addAction(QStringLiteral("All'indietro (più volte: più veloce)"), this, [this] { shuttle(-1); })
        ->setShortcut(QKeySequence(Qt::Key_J));
    playMenu->addAction(QStringLiteral("Pausa"), this, [this] { setPlaying(false); })->setShortcut(QKeySequence(Qt::Key_K));
    playMenu->addAction(QStringLiteral("S&top (torna dove era partita la riproduzione)"), this, [this] { stopPlayback(); });
    playMenu->addSeparator();
    QAction* previousFrame = playMenu->addAction(QStringLiteral("Fotogramma p&recedente"), this, [this] { stepFrame(-1); });
    previousFrame->setShortcuts({QKeySequence(Qt::Key_Left), QKeySequence(Qt::Key_Comma)});
    QAction* nextFrame = playMenu->addAction(QStringLiteral("Fotogramma &successivo"), this, [this] { stepFrame(+1); });
    nextFrame->setShortcuts({QKeySequence(Qt::Key_Right), QKeySequence(Qt::Key_Period)});
    playMenu->addAction(QStringLiteral("Fotogramma chiave (Sorgente) / taglio (Montaggio) precedente"), this,
                        [this] { stepToMark(-1); })
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Left));
    playMenu->addAction(QStringLiteral("Fotogramma chiave (Sorgente) / taglio (Montaggio) successivo"), this,
                        [this] { stepToMark(+1); })
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Right));
    QAction* backAction = playMenu->addAction(QStringLiteral("&Indietro di 5 secondi"), this, [this] { seekRelative(-5.0); });
    backAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    QAction* forwardAction = playMenu->addAction(QStringLiteral("&Avanti di 5 secondi"), this, [this] { seekRelative(5.0); });
    forwardAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    QAction* startAction = playMenu->addAction(QStringLiteral("Vai all'i&nizio"), this, [this] { seekStart(); });
    startAction->setShortcut(QKeySequence(Qt::Key_Home));
    QAction* endAction = playMenu->addAction(QStringLiteral("Vai alla &fine"), this, [this] { seekEnd(); });
    endAction->setShortcut(QKeySequence(Qt::Key_End));
    playMenu->addSeparator();
    // Zoom della linea del tempo dell'anteprima attiva (anche Ctrl+rotellina sulla linea del tempo).
    playMenu->addAction(QStringLiteral("Ingrandisci la linea del tempo"), this, [this] { zoomTimeline(1.25); })
        ->setShortcuts({QKeySequence(Qt::Key_Plus), QKeySequence(Qt::Key_Equal)});
    playMenu->addAction(QStringLiteral("Riduci la linea del tempo"), this, [this] { zoomTimeline(1.0 / 1.25); })
        ->setShortcut(QKeySequence(Qt::Key_Minus));
    playMenu->addAction(QStringLiteral("Adatta la linea del tempo (mostra tutto)"), this, [this] { zoomTimeline(0.0); });

    QMenu* segmentMenu = menuBar()->addMenu(QStringLiteral("&Segmento"));
    // Con il Montaggio attivo, I e O segnano il tratto della corsia audio.
    QAction* inAction = segmentMenu->addAction(QStringLiteral("Segna &IN (inizio)"), this, [this] {
        if (programActive_)
            markAudioIn();
        else
            player_->markIn();
    });
    inAction->setShortcut(QKeySequence(Qt::Key_I));
    inAction->setStatusTip(QStringLiteral("Nella Sorgente: inizio del pezzo. Nel Montaggio: inizio del tratto audio."));
    QAction* outAction = segmentMenu->addAction(QStringLiteral("Segna &OUT (fine)"), this, [this] {
        if (programActive_)
            markAudioOut();
        else
            player_->markOut();
    });
    outAction->setShortcut(QKeySequence(Qt::Key_O));
    outAction->setStatusTip(QStringLiteral("Nella Sorgente: fine del pezzo. Nel Montaggio: fine del tratto audio."));
    segmentMenu->addSeparator();
    QAction* goIn = segmentMenu->addAction(QStringLiteral("Vai a IN"), this, [this] { player_->goToIn(); });
    goIn->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_I));
    QAction* goOut = segmentMenu->addAction(QStringLiteral("Vai a OUT"), this, [this] { player_->goToOut(); });
    goOut->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_O));
    segmentMenu->addSeparator();
    segmentMenu->addAction(QStringLiteral("&Cancella IN e OUT"), this, [this] { player_->clearSegment(); });

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Montaggio"));
    QAction* add = editMenu->addAction(QStringLiteral("&Aggiungi IN-OUT al montaggio"), this, [this] { addToSequence(); });
    add->setShortcut(QKeySequence(Qt::Key_A));
    add->setStatusTip(QStringLiteral("Dopo il pezzo selezionato, o in fondo; senza IN e OUT tutto il file"));
    QAction* cut = editMenu->addAction(QStringLiteral("&Togli IN-OUT dal montaggio"), this, [this] { cutFromSequence(); });
    cut->setShortcut(QKeySequence(Qt::Key_X));
    QAction* split = editMenu->addAction(QStringLiteral("&Dividi il pezzo al cursore"), this, [this] { splitAtPlayhead(); });
    split->setShortcut(QKeySequence(Qt::Key_S));
    QAction* remove = editMenu->addAction(QStringLiteral("&Elimina il pezzo selezionato"), this, [this] { deleteSelection(); });
    remove->setShortcut(QKeySequence(QKeySequence::Delete));
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("&Svuota il montaggio"), this, [this] { clearSequence(); });
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("&Impostazioni del progetto..."), this, [this] { projectSettingsDialog(); })
        ->setStatusTip(QStringLiteral("Il formato del file esportato: codec, risoluzione, fotogrammi al secondo, audio"));

    QMenu* audioMenu = menuBar()->addMenu(QStringLiteral("A&udio"));
    QAction* mute = audioMenu->addAction(QStringLiteral("&Togli l'audio dal tratto selezionato"), this, [this] { muteSelection(); });
    mute->setShortcut(QKeySequence(Qt::Key_M));
    mute->setStatusTip(QStringLiteral("Il tratto si seleziona trascinando sulla corsia audio, o con I e O nel Montaggio"));
    QAction* unmute = audioMenu->addAction(QStringLiteral("&Rimetti l'audio nel tratto selezionato"), this,
                                           [this] { unmuteSelection(); });
    unmute->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_M));
    audioMenu->addSeparator();
    QAction* replace = audioMenu->addAction(QStringLiteral("&Sostituisci l'audio..."), this, &MainWindow::replaceAudioDialog);
    replace->setStatusTip(QStringLiteral("Un file audio (es. MP3 o M4A) al posto dell'audio originale di tutto il montaggio"));
    originalAudioAction_ = audioMenu->addAction(QStringLiteral("Usa l'audio &originale"), this, [this] { restoreOriginalAudio(); });
    originalAudioAction_->setEnabled(false);

    QMenu* windowMenu = menuBar()->addMenu(QStringLiteral("Fi&nestra"));
    for (QDockWidget* dock : docks()) {
        QAction* toggle = dock->toggleViewAction();
        toggle->setText(QStringLiteral("Pannello %1").arg(dock->windowTitle()));
        windowMenu->addAction(toggle);
    }
    windowMenu->addSeparator();
    windowMenu->addAction(QStringLiteral("Schermo intero della Sorgente"), this, [this] { toggleFullScreen(sourceDock_); });
    windowMenu->addAction(QStringLiteral("Schermo intero del Montaggio"), this, [this] { toggleFullScreen(programDock_); });
    QAction* exitFull = windowMenu->addAction(QStringLiteral("Esci dallo schermo intero"), this, [this] { exitFullScreen(); });
    exitFull->setShortcut(QKeySequence(Qt::Key_Escape));
    windowMenu->addSeparator();
    windowMenu->addAction(QStringLiteral("Riaggancia la &linea del tempo"), this, [this] { redockTimeline(); });
    windowMenu->addAction(QStringLiteral("&Ripristina disposizione"), this, [this] { resetLayout(); });

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Aiuto"));
    helpMenu->addAction(QStringLiteral("&Scorciatoie da tastiera"), this, &MainWindow::showShortcuts)
        ->setShortcut(QKeySequence(Qt::Key_F1));
    helpMenu->addAction(QStringLiteral("&Informazioni su EditorVideo"), this, &MainWindow::showAbout);
    updateExportAction();
    updateUndoActions();

    // Le scorciatoie valgono anche in un pannello staccato (che è una finestra a sé) o a schermo intero.
    QList<QAction*> shortcuts;
    for (QAction* menu : menuBar()->actions()) {
        if (menu->menu() == nullptr)
            continue;
        for (QAction* action : menu->menu()->actions()) {
            if (!action->shortcuts().isEmpty())
                shortcuts.append(action);
        }
    }
    for (QDockWidget* dock : docks())
        dock->addActions(shortcuts);
}

// ---------------------------------------------------------------------------------------------------------------
// File del progetto

void MainWindow::openFile()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Apri file video"), QString(), kVideoFilter);
    if (!path.isEmpty())
        openPath(path);
}

void MainWindow::addFilesDialog()
{
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, QStringLiteral("Aggiungi file al progetto"), QString(), kVideoFilter);
    if (!paths.isEmpty())
        addFiles(paths);
}

void MainWindow::resetProject()
{
    // Si ferma un'eventuale analisi in corso e si riparte da zero.
    if (probeThread_ != nullptr) {
        cancelProbe_ = true;
        probeThread_->wait();
    }
    probeQueue_.clear();
    project_.clear();
    clipInfo_.clear();
    signatures_.clear();
    waveforms_->clearQueue();  // le forme d'onda già calcolate restano: servono se si riapre lo stesso file
    timeline_->setAudioSelection(0.0, 0.0);
    currentClipId_ = 0;
    currentPath_.clear();
    pendingView_.reset();
    savedDurations_.clear();
    projectPath_.clear();
    history_.clear();
    updateUndoActions();
    player_->closeFile();
    proxies_->setFolder(proxyFolder());
}

void MainWindow::openPath(const QString& path)
{
    // Nuovo progetto con un solo file: finché non si monta niente non c'è niente da salvare.
    loading_ = true;
    resetProject();
    sequenceChanged();
    addFiles({path});
    loading_ = false;
    history_.clear();
    setModified(false);
}

void MainWindow::addFiles(const QStringList& paths)
{
    const core::Project before = project_;
    int first = 0;
    for (const QString& path : paths) {
        const std::string utf8 = nativeAbsolute(path).toStdString();
        const int id = project_.add(utf8);
        if (first == 0)
            first = id;
        probeQueue_.push_back(id);
        waveforms_->request(utf8);
    }
    refreshClipList();
    if (currentClipId_ == 0 && first != 0)
        selectClip(first);
    updateProxies();
    startNextProbe();
    updateExportAction();
    if (!loading_) {
        recordHistory(paths.size() > 1 ? QStringLiteral("Aggiungi file") : QStringLiteral("Aggiungi il file"), before);
        setModified(true);
    }
}

void MainWindow::startNextProbe()
{
    if (probeThread_ != nullptr || probeQueue_.empty())
        return;
    const int id = probeQueue_.front();
    probeQueue_.pop_front();
    const core::Clip* clip = project_.find(id);
    if (clip == nullptr) {
        startNextProbe();
        return;
    }
    const std::string utf8Path = clip->path;
    auto outcome = std::make_shared<ProbeOutcome>();

    cancelProbe_ = false;
    probingClipId_ = id;
    probeThread_ = QThread::create([utf8Path, outcome, cancel = &cancelProbe_] {
        try {
            outcome->info = core::probeFile(utf8Path, cancel);
        } catch (const core::ProbeCancelled&) {
            outcome->cancelled = true;
        } catch (const std::exception& e) {
            outcome->error = QString::fromUtf8(e.what());
        }
    });
    probeThread_->setParent(this);

    connect(probeThread_, &QThread::finished, this, [this, outcome, id] {
        probeThread_->deleteLater();
        probeThread_ = nullptr;
        probingClipId_ = 0;
        QApplication::restoreOverrideCursor();

        if (!outcome->cancelled && project_.find(id) != nullptr) {
            if (!outcome->info) {
                const QString name = fileNameOf(project_.find(id)->path);
                if (id == currentClipId_)
                    keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> non disponibili"));
                removeClip(id);
                statusBar()->showMessage(QStringLiteral("Impossibile aprire %1").arg(name));
                QMessageBox::warning(this, QStringLiteral("Impossibile aprire il file"), outcome->error);
            } else {
                const core::MediaInfo& info = *outcome->info;
                clipInfo_[id] = info;
                project_.setAnalysis(id, info.keyframeTimes, info.durationSeconds);
                if (id == currentClipId_) {
                    showInfo(info);
                    player_->setKeyframes(info.keyframeTimes);
                    updateSourceBands();
                    // IN e OUT salvati nel progetto: si possono rimettere ora che si conoscono i fotogrammi chiave.
                    if (pendingView_ && pendingView_->sourceClipId == id) {
                        if (pendingView_->in && pendingView_->out)
                            player_->setInOut(*pendingView_->in, *pendingView_->out);
                        player_->seekTo(pendingView_->sourcePosition);
                        pendingView_.reset();
                    }
                }
                // Il file è cambiato dall'ultimo salvataggio del progetto?
                if (const auto saved = savedDurations_.find(id); saved != savedDurations_.end()) {
                    const double before = saved->second;
                    savedDurations_.erase(saved);
                    if (std::abs(before - info.durationSeconds) > 0.05) {
                        QMessageBox::warning(this, QStringLiteral("File cambiato"),
                                             QStringLiteral("%1 è cambiato dall'ultimo salvataggio del progetto: ora dura %2 "
                                                            "invece di %3.\nControlla i pezzi del montaggio che lo usano.")
                                                 .arg(fileNameOf(project_.find(id)->path), timeText(info.durationSeconds),
                                                      timeText(before)));
                    }
                }
                refreshClipList();
                // Le miniature di un file si chiedono solo dopo l'analisi: la timeline deve ridisegnare i suoi blocchi.
                timeline_->thumbnailsChanged();
                updateExportAction();
            }
        }
        startNextProbe();
    });

    QApplication::setOverrideCursor(Qt::BusyCursor);
    statusBar()->showMessage(QStringLiteral("Analisi di %1 in corso...").arg(fileNameOf(utf8Path)));
    probeThread_->start();
}

void MainWindow::selectClip(int id)
{
    const core::Clip* clip = project_.find(id);
    if (clip == nullptr || id == currentClipId_)
        return;

    currentClipId_ = id;
    currentPath_ = qs(clip->path);
    player_->openFile(currentPath_);
    if (clip->analyzed) {
        showInfo(clipInfo_[id]);
        player_->setKeyframes(clip->keyframes());
        updateSourceBands();
    } else {
        infoTree_->clear();
        keyframeList_->clear();
        keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> analisi in corso..."));
        updateTitle();
    }
    refreshClipList();
    updateExportAction();
}

void MainWindow::removeClip(int id, const QString& undoLabel)
{
    const int index = project_.indexOf(id);
    if (index < 0)
        return;
    if (!undoLabel.isEmpty()) {
        recordHistory(undoLabel, project_);
    }
    std::erase(probeQueue_, id);
    if (id == probingClipId_)
        cancelProbe_ = true;  // l'analisi finisce da sola e il risultato viene ignorato
    project_.remove(id);      // toglie anche i suoi pezzi dal montaggio (le informazioni restano, per Annulla)

    if (id == currentClipId_) {
        currentClipId_ = 0;
        currentPath_.clear();
        player_->closeFile();
        if (!project_.empty()) {
            const int next = project_.clips()[std::min<std::size_t>(index, project_.clips().size() - 1)].id;
            selectClip(next);
        } else {
            infoTree_->clear();
            keyframeList_->clear();
            updateTitle();
        }
    }
    sequenceChanged();
}

int MainWindow::selectedClipId() const
{
    const QListWidgetItem* item = clipList_->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toInt() : 0;
}

void MainWindow::refreshClipList()
{
    if (clipList_ == nullptr)
        return;
    updatingList_ = true;
    std::map<int, int> uses;  // pezzi nel montaggio per file
    for (const core::Fragment& f : project_.sequence().fragments())
        ++uses[f.clipId];
    // Con gli stessi file nello stesso ordine si aggiornano le righe che ci sono (restano scorrimento e selezione),
    // altrimenti si rifà l'elenco.
    const auto& clips = project_.clips();
    bool sameFiles = clipList_->count() == static_cast<int>(clips.size());
    for (int row = 0; sameFiles && row < clipList_->count(); ++row)
        sameFiles = clipList_->item(row)->data(Qt::UserRole).toInt() == clips[static_cast<std::size_t>(row)].id;
    if (!sameFiles)
        clipList_->clear();
    QListWidgetItem* current = nullptr;
    for (std::size_t row = 0; row < clips.size(); ++row) {
        const core::Clip& clip = clips[row];
        const auto found = uses.find(clip.id);
        const int count = found != uses.end() ? found->second : 0;
        QString detail = clip.analyzed ? timeText(clip.duration) : QStringLiteral("analisi in corso...");
        if (count > 0)
            detail += QStringLiteral(", %1 pezzi nel montaggio").arg(count);
        if (const int divisor = proxies_->divisor(); divisor > 1) {
            const QString path = qs(clip.path);
            if (const double progress = proxies_->progress(path); progress >= 0)
                detail += QStringLiteral(", proxy 1/%1: %2%").arg(divisor).arg(static_cast<int>(progress * 100));
            else if (proxies_->hasProxy(path))
                detail += QStringLiteral(", proxy 1/%1").arg(divisor);
        }
        const QString text = QStringLiteral("%1  —  %2").arg(fileNameOf(clip.path), detail);
        QListWidgetItem* item = sameFiles ? clipList_->item(static_cast<int>(row)) : new QListWidgetItem;
        if (item->text() != text)
            item->setText(text);
        item->setData(Qt::UserRole, clip.id);
        item->setToolTip(qs(clip.path));
        // Lo stesso colore dei suoi blocchi nel montaggio.
        item->setForeground(blockColor(project_.indexOf(clip.id)));
        if (!sameFiles)
            clipList_->addItem(item);
        if (clip.id == currentClipId_)
            current = item;
    }
    if (clipList_->currentItem() != current)
        clipList_->setCurrentItem(current);
    updatingList_ = false;
}

void MainWindow::addKeyframeRows()
{
    constexpr std::size_t kBatch = 500;
    // Toglie la riga "Mostra altri" (se c'è): l'ultima.
    const int last = keyframeList_->topLevelItemCount() - 1;
    if (last >= 0 && keyframeList_->topLevelItem(last)->data(0, Qt::UserRole).toBool())
        delete keyframeList_->takeTopLevelItem(last);
    const std::size_t from = static_cast<std::size_t>(keyframeList_->topLevelItemCount());
    const std::size_t to = std::min(keyframeTimes_.size(), from + kBatch);
    QList<QTreeWidgetItem*> rows;
    rows.reserve(static_cast<qsizetype>(to - from) + 1);
    for (std::size_t i = from; i < to; ++i) {
        auto* row = new QTreeWidgetItem({QString::number(i + 1), qs(core::formatDuration(keyframeTimes_[i]))});
        row->setData(1, Qt::UserRole, keyframeTimes_[i]);
        row->setToolTip(1, QStringLiteral("Clic per portare l'anteprima qui"));
        rows.append(row);
    }
    if (to < keyframeTimes_.size()) {
        auto* more = new QTreeWidgetItem({QString(), QStringLiteral("Mostra altri (%1 da vedere)").arg(keyframeTimes_.size() - to)});
        more->setData(0, Qt::UserRole, true);
        rows.append(more);
    }
    keyframeList_->addTopLevelItems(rows);
}

void MainWindow::showInfo(const core::MediaInfo& info)
{
    const QString path = qs(info.path);
    const QString fileName = QFileInfo(path).fileName();
    updateTitle();

    infoTree_->clear();

    auto* file = new QTreeWidgetItem(infoTree_, {QStringLiteral("File"), fileName});
    addRow(file, QStringLiteral("Percorso"), path);
    addRow(file, QStringLiteral("Formato contenitore"),
           QStringLiteral("%1  (%2)").arg(qs(info.formatLongName), qs(info.formatName)));
    addRow(file, QStringLiteral("Durata"), qs(core::formatDuration(info.durationSeconds)));
    addRow(file, QStringLiteral("Bitrate complessivo"), qs(core::formatBitRate(info.bitRate)));
    addRow(file, QStringLiteral("Flussi"), QString::number(info.streams.size()));
    file->setExpanded(true);

    for (const core::StreamInfo& stream : info.streams)
        addStream(infoTree_, stream);

    keyframeList_->clear();
    keyframeTimes_.clear();
    if (info.keyframeStreamIndex >= 0) {
        QString title = QStringLiteral("<b>Fotogrammi chiave: %1</b> (flusso %2)")
                            .arg(info.keyframeTimes.size())
                            .arg(info.keyframeStreamIndex);
        if (info.keyframeTimes.size() > 1) {
            const double first = info.keyframeTimes.front();
            const double last = info.keyframeTimes.back();
            title += QStringLiteral("<br>dal %1 al %2, uno ogni %3 s in media")
                         .arg(qs(core::formatDuration(first)), qs(core::formatDuration(last)))
                         .arg((last - first) / static_cast<double>(info.keyframeTimes.size() - 1), 0, 'f', 2);
        }
        keyframeTitle_->setText(title);
        // Con migliaia di fotogrammi chiave l'elenco si riempie a blocchi (la riga in fondo ne aggiunge altri).
        keyframeTimes_ = info.keyframeTimes;
        addKeyframeRows();
    } else {
        keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> nessun flusso video"));
    }

    statusBar()->showMessage(QStringLiteral("%1  ·  %2  ·  %3 fotogrammi chiave")
                                 .arg(fileName, qs(core::formatDuration(info.durationSeconds)))
                                 .arg(info.keyframeTimes.size()));
}

// ---------------------------------------------------------------------------------------------------------------
// Progetto salvato

void MainWindow::setModified(bool modified)
{
    modified_ = modified;
    updateTitle();
}

void MainWindow::updateTitle()
{
    // "montaggio.evproj* · gita.mp4 - EditorVideo": l'asterisco dice che ci sono modifiche non salvate.
    QString title = projectPath_.isEmpty() ? QStringLiteral("Nuovo progetto") : QFileInfo(projectPath_).fileName();
    title += QStringLiteral("[*]");
    if (!currentPath_.isEmpty())
        title += QStringLiteral(" · %1").arg(QFileInfo(currentPath_).fileName());
    setWindowTitle(title + QStringLiteral(" - EditorVideo"));
    setWindowModified(modified_);
}

core::ProjectView MainWindow::currentView() const
{
    core::ProjectView view;
    view.sourceClipId = currentClipId_;
    const core::CutSegment& segment = player_->segment();
    if (segment.hasIn())
        view.in = segment.in();
    if (segment.hasOut())
        view.out = segment.out();
    view.sourcePosition = player_->currentPosition();
    view.programPosition = program_->position();
    return view;
}

bool MainWindow::maybeSave()
{
    if (!modified_ || project_.empty())
        return true;
    QMessageBox box(QMessageBox::Question, QStringLiteral("Modifiche non salvate"),
                    QStringLiteral("Il progetto %1 ha modifiche non salvate. Vuoi salvarle?")
                        .arg(projectPath_.isEmpty() ? QString() : QFileInfo(projectPath_).fileName()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.button(QMessageBox::Save)->setText(QStringLiteral("Salva"));
    box.button(QMessageBox::Discard)->setText(QStringLiteral("Non salvare"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("Annulla"));
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:
        return saveProject();
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    if (fullScreenDock_ != nullptr)
        exitFullScreen();
    saveLayout();
    event->accept();
}

// ---------------------------------------------------------------------------------------------------------------
// Pannelli

void MainWindow::resetLayout()
{
    if (fullScreenDock_ != nullptr)
        exitFullScreen();
    // A sinistra i file, al centro Sorgente e Montaggio affiancati, sotto la linea del tempo del montaggio.
    for (QDockWidget* dock : docks()) {
        dock->setFloating(false);
        removeDockWidget(dock);
    }
    addDockWidget(Qt::TopDockWidgetArea, filesDock_);
    splitDockWidget(filesDock_, sourceDock_, Qt::Horizontal);
    splitDockWidget(sourceDock_, programDock_, Qt::Horizontal);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
    for (QDockWidget* dock : docks())
        dock->show();
    resizeDocks({filesDock_, sourceDock_, programDock_}, {320, 590, 590}, Qt::Horizontal);
    resizeDocks({sourceDock_, timelineDock_}, {620, 230}, Qt::Vertical);
    // La linea del tempo alta quella iniziale anche se la finestra non ha posto per 620 + 230 (il resto va sopra).
    timelineHeight_ = 230;
    timelineRedocking_ = true;
    QTimer::singleShot(0, this, [this] { applyTimelineHeight(); });
}

void MainWindow::saveLayout()
{
    QSettings s = settings();
    s.setValue(QStringLiteral("finestra/geometria"), saveGeometry());
    s.setValue(QStringLiteral("finestra/disposizione"), saveState());
    s.setValue(QStringLiteral("finestra/altezzaLineaDelTempo"), timelineHeight_);
}

void MainWindow::restoreLayout()
{
    QSettings s = settings();
    const QByteArray geometry = s.value(QStringLiteral("finestra/geometria")).toByteArray();
    const QByteArray state = s.value(QStringLiteral("finestra/disposizione")).toByteArray();
    timelineHeight_ = std::max(230, s.value(QStringLiteral("finestra/altezzaLineaDelTempo"), 230).toInt());
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    if (!state.isEmpty() && !restoreState(state))
        resetLayout();
    // Una disposizione salvata prima che la linea del tempo potesse stare solo in basso: la si rimette lì.
    if (!timelineDock_->isFloating() && dockWidgetArea(timelineDock_) != Qt::BottomDockWidgetArea)
        redockTimeline();
    // Il monitor dove stava la finestra (o un pannello staccato) potrebbe non esserci più.
    keepOnScreen(this);
    for (QDockWidget* dock : docks()) {
        if (dock->isFloating())
            keepOnScreen(dock);
    }
}

void MainWindow::keepOnScreen(QWidget* window)
{
    const QRect frame = window->frameGeometry();
    for (const QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(frame.adjusted(40, 10, -40, -10)))
            return;
    }
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    QSize size = window->size().boundedTo(available.size() - QSize(40, 60));
    window->resize(size);
    window->move(available.center() - QPoint(size.width() / 2, size.height() / 2));
}

QDockWidget* MainWindow::dockedPanelAt(const QPoint& pos) const
{
    for (QDockWidget* dock : docks()) {
        if (dock->isVisible() && !dock->isFloating() && dock->geometry().contains(pos))
            return dock;
    }
    return nullptr;
}

bool MainWindow::findCorner(const QPoint& pos, CornerDrag& corner) const
{
    // Un incrocio è un punto fra i pannelli (non dentro uno) con pannelli diversi a sinistra e a destra (separatore
    // verticale) e sopra e sotto (separatore orizzontale), entro qualche pixel.
    constexpr int d = 7;
    if (dockedPanelAt(pos) != nullptr)
        return false;
    QDockWidget* tl = dockedPanelAt(pos + QPoint(-d, -d));
    QDockWidget* tr = dockedPanelAt(pos + QPoint(d, -d));
    QDockWidget* bl = dockedPanelAt(pos + QPoint(-d, d));
    QDockWidget* br = dockedPanelAt(pos + QPoint(d, d));
    CornerDrag found;
    if (tl != nullptr && tr != nullptr && tl != tr) {
        found.left = tl;
        found.right = tr;
    } else if (bl != nullptr && br != nullptr && bl != br) {
        found.left = bl;
        found.right = br;
    }
    if (tl != nullptr && bl != nullptr && tl != bl) {
        found.top = tl;
        found.bottom = bl;
    } else if (tr != nullptr && br != nullptr && tr != br) {
        found.top = tr;
        found.bottom = br;
    }
    if (found.left == nullptr || found.top == nullptr)
        return false;
    found.start = pos;
    found.leftWidth = found.left->width();
    found.rightWidth = found.right->width();
    found.topHeight = found.top->height();
    found.bottomHeight = found.bottom->height();
    corner = found;
    return true;
}

bool MainWindow::event(QEvent* event)
{
    // Incroci fra separatori: QMainWindow sposta un separatore alla volta; qui, trascinando il punto in cui due si
    // incrociano, si spostano tutti e due (larghezza e altezza insieme).
    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = mouse->position().toPoint();
        if (cornerDrag_) {
            const QPoint delta = pos - cornerDrag_->start;
            resizeDocks({cornerDrag_->left, cornerDrag_->right},
                        {std::max(1, cornerDrag_->leftWidth + delta.x()), std::max(1, cornerDrag_->rightWidth - delta.x())},
                        Qt::Horizontal);
            resizeDocks({cornerDrag_->top, cornerDrag_->bottom},
                        {std::max(1, cornerDrag_->topHeight + delta.y()), std::max(1, cornerDrag_->bottomHeight - delta.y())},
                        Qt::Vertical);
            return true;
        }
        CornerDrag corner;
        const bool onCorner = mouse->buttons() == Qt::NoButton && findCorner(pos, corner);
        if (onCorner != cornerCursor_) {
            cornerCursor_ = onCorner;
            if (onCorner)
                setCursor(Qt::SizeAllCursor);
            else
                unsetCursor();
        }
        if (onCorner)
            return true;
        break;
    }
    case QEvent::MouseButtonPress: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        CornerDrag corner;
        if (mouse->button() == Qt::LeftButton && findCorner(mouse->position().toPoint(), corner)) {
            cornerDrag_ = corner;
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        if (cornerDrag_) {
            cornerDrag_.reset();
            return true;
        }
        break;
    case QEvent::Leave:
        if (cornerCursor_ && !cornerDrag_) {
            cornerCursor_ = false;
            unsetCursor();
        }
        break;
    default:
        break;
    }
    return QMainWindow::event(event);
}

void MainWindow::redockTimeline()
{
    if (fullScreenDock_ == timelineDock_)
        exitFullScreen();
    if (debugDock())
        std::fprintf(stderr, "redock: floating %d area %d w %d\n", timelineDock_->isFloating(),
                     static_cast<int>(dockWidgetArea(timelineDock_)), timelineDock_->width());
    timelinePoll_->stop();
    showDockPreview(false);
    timelineRedocking_ = true;
    // Sempre la stessa sequenza, qualunque sia lo stato di prima: tolta, aggiunta in basso, agganciata, visibile.
    // Mai una finestra nativa da agganciata (vedi startTimelineDrag).
    timelineDock_->setAttribute(Qt::WA_NativeWindow, false);
    removeDockWidget(timelineDock_);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_, Qt::Horizontal);
    timelineDock_->setFloating(false);
    timelineDock_->show();
    // L'altezza si sistema dopo che Qt ha rifatto la disposizione: subito verrebbe ignorata.
    QTimer::singleShot(0, this, [this] { applyTimelineHeight(); });
}

void MainWindow::applyTimelineHeight()
{
    // La disposizione si ricalcola subito, prima di dare l'altezza (e dopo si ridisegna tutto).
    if (layout() != nullptr) {
        layout()->invalidate();
        layout()->activate();
    }
    // L'ultima altezza da agganciata, almeno quella iniziale e al massimo metà della finestra.
    const int maximum = std::max(230, height() / 2);
    const int wanted = std::clamp(timelineHeight_, 230, maximum);
    // Con il solo pannello in basso Qt non sa da chi togliere lo spazio: gli si dà anche un pannello di sopra, che
    // prende il resto.
    QDockWidget* above = nullptr;
    for (QDockWidget* dock : {sourceDock_, programDock_, filesDock_}) {
        if (dock->isVisible() && !dock->isFloating()) {
            above = dock;
            break;
        }
    }
    if (above != nullptr) {
        const int total = above->height() + timelineDock_->height();
        resizeDocks({above, timelineDock_}, {std::max(1, total - wanted), wanted}, Qt::Vertical);
    } else {
        resizeDocks({timelineDock_}, {wanted}, Qt::Vertical);
    }
    if (layout() != nullptr)
        layout()->activate();
    update();
    timelineRedocking_ = false;
    timelineHeight_ = timelineDock_->height();
}

bool MainWindow::isTitleHighlighted(QDockWidget* dock) const
{
    return dock->property("titoloEvidenziato").toBool();
}

void MainWindow::setTitleHighlight(QDockWidget* dock, bool highlighted)
{
    if (isTitleHighlighted(dock) == highlighted)
        return;
    dock->setProperty("titoloEvidenziato", highlighted);
    // Il foglio di stile guarda la proprietà: va riapplicato.
    dock->style()->unpolish(dock);
    dock->style()->polish(dock);
    dock->update();
    if (highlighted)
        dock->setCursor(Qt::OpenHandCursor);
    else
        dock->unsetCursor();
}

bool MainWindow::updateTitleHover(QDockWidget* dock, QEvent* event)
{
    const auto inTitle = [dock](const QPointF& pos) {
        // Il titolo è la striscia sopra il contenuto del pannello.
        return !dock->isFloating() && dock->widget() != nullptr && pos.y() >= 0 && pos.y() < dock->widget()->geometry().top();
    };
    switch (event->type()) {
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        setTitleHighlight(dock, inTitle(static_cast<QHoverEvent*>(event)->position()));
        return true;
    case QEvent::HoverLeave:
        setTitleHighlight(dock, false);
        return true;
    case QEvent::MouseButtonPress:
        if (isTitleHighlighted(dock) && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
            dock->setCursor(Qt::ClosedHandCursor);  // si trascina
        return true;
    case QEvent::MouseButtonRelease:
        if (isTitleHighlighted(dock))
            dock->setCursor(Qt::OpenHandCursor);
        return true;
    default:
        return false;
    }
}

void MainWindow::timelinePollStep(bool buttonDown, const QPoint& cursor)
{
    if (!timelineDock_->isFloating() || fullScreenDock_ != nullptr) {
        timelinePoll_->stop();
        showDockPreview(false);
        return;
    }
    if (buttonDown && !pollButtonDown_)
        pollPressPos_ = timelineDock_->pos();  // inizio di un trascinamento (o di un clic qualsiasi)
    const QRect window = frameGeometry();
    const bool low = window.contains(cursor) && cursor.y() >= window.top() + window.height() / 2;
    // Spostata: lo dice Windows (ha fatto lui lo spostamento) oppure la posizione (prove senza Windows).
    const bool moved = movedByWindows_ || timelineDock_->pos() != pollPressPos_;
    // Durante il trascinamento, con il cursore nella metà bassa: si vede dove andrà.
    showDockPreview(buttonDown && moved && low);
    // Rilasciato dopo averla spostata: è stata lasciata lì.
    const bool released = !buttonDown && pollButtonDown_ && moved;
    pollButtonDown_ = buttonDown;
    if (!buttonDown)
        movedByWindows_ = false;
    if (released) {
        showDockPreview(false);
        // Il pulsante si è alzato, ma chi sta trascinando (Qt, o Windows) può non averlo ancora saputo: si aspetta
        // che abbia finito, altrimenti alla fine del suo trascinamento la staccherebbe di nuovo.
        QTimer::singleShot(30, this, [this, cursor] { finishTimelineDrop(cursor, 40); });
    }
}

QPoint MainWindow::cursorNow() const
{
    return testCursor_ ? *testCursor_ : QCursor::pos();
}

void MainWindow::startTimelineDrag(const QPoint& global, const QPoint& local)
{
    // Staccata qui, larga al massimo due terzi della finestra, con il punto preso sotto il cursore; poi lo spostamento
    // lo fa Windows (come trascinando il titolo di una finestra), che a fine spostamento manda WM_EXITSIZEMOVE.
    const int w = std::min(timelineDock_->width(), std::max(420, width() * 2 / 3));
    const int h = timelineDock_->height();
    timelineDock_->setFloating(true);
    timelineDock_->resize(w, h);
    const int grabX = std::clamp(local.x(), 40, w - 40);
    timelineDock_->move(global - QPoint(grabX, 12));
    timelineDock_->show();
    QCoreApplication::processEvents();  // la finestra nativa deve esistere ed essere al suo posto
    // L'handle della finestra (staccata, quindi di primo livello) senza QWidget::winId(), che renderebbe il pannello una
    // finestra nativa per sempre: riagganciato resterebbe una finestra figlia di Windows, ridisegnata e messa al suo posto
    // solo quando la finestra principale si riorganizza.
    if (timelineDock_->windowHandle() == nullptr)
        return;
    void* hwnd = reinterpret_cast<void*>(timelineDock_->windowHandle()->winId());
    if (nativeDragForTest_) {
        nativeDragForTest_(hwnd, global);
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);  // in pixel fisici, come vuole Windows
    ReleaseCapture();       // il mouse non è più di Qt: lo prende Windows
    SendMessageW(static_cast<HWND>(hwnd), WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
}

void MainWindow::finishTimelineDrop(const QPoint& cursor, int attempts)
{
    if (debugDock())
        std::fprintf(stderr, "finish: floating %d grabber %p buttons %d moveloop %d area %d w %d\n", timelineDock_->isFloating(),
                     static_cast<void*>(QWidget::mouseGrabber()), static_cast<int>(QGuiApplication::mouseButtons()),
                     nativeMoveLoop_, static_cast<int>(dockWidgetArea(timelineDock_)), timelineDock_->width());
    if (!timelineDock_->isFloating() || fullScreenDock_ != nullptr)
        return;
    const bool dragging = nativeMoveLoop_ || QWidget::mouseGrabber() != nullptr;
    if (dragging) {
        if (attempts > 0)
            QTimer::singleShot(25, this, [this, cursor, attempts] { finishTimelineDrop(cursor, attempts - 1); });
        return;
    }
    if (!redockTimelineIfDroppedLow(cursor, false))
        return;
    // Controllo: se qualcosa l'ha staccata di nuovo subito dopo, si riprova (qualche volta).
    QTimer::singleShot(80, this, [this, cursor, attempts] {
        if (timelineDock_->isFloating() && attempts > 0)
            finishTimelineDrop(cursor, attempts - 1);
    });
}

void MainWindow::showDockPreview(bool visible)
{
    if (!visible) {
        if (dockPreview_ != nullptr)
            dockPreview_->hide();
        return;
    }
    if (dockPreview_ == nullptr) {
        dockPreview_ = new QWidget(this);
        dockPreview_->setAttribute(Qt::WA_TransparentForMouseEvents);
        dockPreview_->setAttribute(Qt::WA_StyledBackground);
        QColor color = palette().color(QPalette::Highlight);
        dockPreview_->setStyleSheet(QStringLiteral("background: rgba(%1, %2, %3, 90); border: 3px solid rgb(%1, %2, %3);")
                                        .arg(color.red())
                                        .arg(color.green())
                                        .arg(color.blue()));
    }
    // La striscia in basso, a tutta larghezza, alta quanto sarà la linea del tempo.
    const int bottom = statusBar()->isVisible() ? statusBar()->geometry().top() : height();
    const int h = std::clamp(timelineHeight_, 230, std::max(230, height() / 2));
    dockPreview_->setGeometry(0, std::max(0, bottom - h), width(), std::min(h, bottom));
    dockPreview_->raise();
    dockPreview_->show();
}

bool MainWindow::redockTimelineIfDroppedLow(const QPoint& cursor, bool buttonDown)
{
    if (buttonDown || !timelineDock_->isFloating() || fullScreenDock_ != nullptr)
        return false;
    const QRect window = frameGeometry();
    if (!window.contains(cursor) || cursor.y() < window.top() + window.height() / 2)
        return false;
    redockTimeline();
    statusBar()->showMessage(QStringLiteral("Linea del tempo riagganciata in basso."), 4000);
    return true;
}

void MainWindow::toggleFullScreen(QDockWidget* dock)
{
    if (fullScreenDock_ != nullptr) {
        const bool same = fullScreenDock_ == dock;
        exitFullScreen();
        if (same)
            return;
    }
    // Il pannello diventa una finestra a sé, a schermo intero e senza la barra del titolo; uscendo si ritrova la
    // disposizione di prima.
    layoutBeforeFullScreen_ = saveState();
    fullScreenDock_ = dock;
    dock->setFloating(true);
    dock->setTitleBarWidget(new QWidget(dock));
    dock->showFullScreen();
    dock->activateWindow();
    statusBar()->showMessage(QStringLiteral("Schermo intero: Esc o doppio clic sull'immagine per uscire."), 5000);
}

void MainWindow::exitFullScreen()
{
    QDockWidget* dock = fullScreenDock_;
    if (dock == nullptr)
        return;
    fullScreenDock_ = nullptr;
    QWidget* title = dock->titleBarWidget();
    dock->setTitleBarWidget(nullptr);
    delete title;
    dock->showNormal();
    restoreState(layoutBeforeFullScreen_);
    activateWindow();
}

bool MainWindow::saveProject()
{
    if (projectPath_.isEmpty())
        return saveProjectAsDialog();
    return saveProjectAs(projectPath_);
}

bool MainWindow::saveProjectAsDialog()
{
    // Si propone la cartella del primo file, con il suo nome.
    QString suggested = projectPath_;
    if (suggested.isEmpty() && !project_.empty()) {
        const QFileInfo first(qs(project_.clips().front().path));
        suggested = QDir(workFolder()).filePath(first.completeBaseName() + kProjectExtension);
    }
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Salva progetto"), suggested, kProjectFilter);
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().compare(kProjectExtension.mid(1), Qt::CaseInsensitive) != 0)
        path += kProjectExtension;
    return saveProjectAs(path);
}

bool MainWindow::saveProjectAs(const QString& path, bool remember)
{
    const QString native = nativeAbsolute(path);
    try {
        core::saveProjectFile(native.toStdString(), project_, currentView());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("Salvataggio non riuscito"), QString::fromUtf8(e.what()));
        return false;
    }
    projectPath_ = native;
    history_.setClean();
    setModified(false);
    if (remember)
        rememberRecent(native);
    // I proxy vanno accanto al progetto (quelli fatti prima di salvare erano nella cartella provvisoria).
    QStringList originals;
    for (const core::Clip& clip : project_.clips())
        originals << qs(clip.path);
    proxies_->moveTo(proxyFolder(), originals);
    player_->reloadVideo();
    program_->reloadVideo();
    proxies_->removeLeftovers();
    updateProxies();
    statusBar()->showMessage(QStringLiteral("Progetto salvato in %1").arg(native), 8000);
    return true;
}

void MainWindow::openProjectDialog()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Apri progetto"),
                                                      projectPath_.isEmpty() ? workFolder() : QFileInfo(projectPath_).path(),
                                                      kProjectFilter);
    if (!path.isEmpty())
        openProject(path);
}

bool MainWindow::openProject(const QString& path, bool interactive)
{
    const QString native = nativeAbsolute(path);
    core::LoadedProject loaded;
    try {
        loaded = core::loadProjectFile(native.toStdString());
    } catch (const std::exception& e) {
        if (interactive)
            QMessageBox::warning(this, QStringLiteral("Impossibile aprire il progetto"), QString::fromUtf8(e.what()));
        statusBar()->showMessage(QString::fromUtf8(e.what()), 10000);
        return false;
    }

    // File che non si trovano più: si possono cercare, oppure si prosegue senza (i loro pezzi vengono tolti).
    while (!loaded.missing.empty()) {
        if (!interactive) {
            core::dropMissing(loaded);
            break;
        }
        QString list;
        for (const core::MissingFile& m : loaded.missing)
            list += QStringLiteral("\n• %1%2  (%3)")
                        .arg(m.clipId == 0 ? QStringLiteral("audio sostitutivo: ") : QString(), qs(m.fileName), qs(m.savedPath));
        QMessageBox box(QMessageBox::Warning, QStringLiteral("File non trovati"),
                        QStringLiteral("Questi file del progetto non si trovano più, né dove erano né accanto al progetto:%1\n\n"
                                       "Puoi cercarli (Trova...) oppure continuare senza: i loro pezzi verranno tolti dal "
                                       "montaggio.")
                            .arg(list),
                        QMessageBox::NoButton, this);
        QPushButton* find = box.addButton(QStringLiteral("Trova %1...").arg(qs(loaded.missing.front().fileName)),
                                          QMessageBox::ActionRole);
        QPushButton* skip = box.addButton(QStringLiteral("Continua senza"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Annulla"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == find) {
            const core::MissingFile missing = loaded.missing.front();
            const QString found = QFileDialog::getOpenFileName(
                this, QStringLiteral("Dov'è %1?").arg(qs(missing.fileName)), QFileInfo(native).path(),
                QStringLiteral("%1 (%1);;Tutti i file (*.*)").arg(qs(missing.fileName)));
            if (found.isEmpty())
                continue;
            core::relocateMissing(loaded, missing, nativeAbsolute(found).toStdString());
            // Gli altri file mancanti potrebbero essere nella stessa cartella.
            const QDir folder = QFileInfo(found).dir();
            for (const core::MissingFile& other : std::vector<core::MissingFile>(loaded.missing)) {
                const QString candidate = folder.filePath(qs(other.fileName));
                if (QFileInfo::exists(candidate))
                    core::relocateMissing(loaded, other, nativeAbsolute(candidate).toStdString());
            }
        } else if (box.clickedButton() == skip) {
            const int removed = core::dropMissing(loaded);
            if (removed > 0)
                QMessageBox::information(this, QStringLiteral("Pezzi tolti"),
                                         QStringLiteral("Tolti dal montaggio %1 pezzi dei file mancanti.").arg(removed));
            break;
        } else {
            return false;
        }
    }

    loading_ = true;
    resetProject();
    project_ = std::move(loaded.project);
    projectPath_ = native;
    savedDurations_ = std::move(loaded.savedDurations);
    pendingView_ = loaded.view;
    for (const core::Clip& clip : project_.clips()) {
        probeQueue_.push_back(clip.id);
        waveforms_->request(clip.path);
    }
    if (const auto& replacement = project_.audioReplacement())
        waveforms_->request(replacement->path);
    sequenceChanged();  // aggiorna anche l'elenco dei file
    const int source = project_.find(loaded.view.sourceClipId) != nullptr ? loaded.view.sourceClipId
                     : project_.empty()                                   ? 0
                                                                          : project_.clips().front().id;
    if (source != 0) {
        selectClip(source);
        player_->seekTo(loaded.view.sourcePosition);
    }
    program_->seekTo(loaded.view.programPosition);
    updateProxies();
    startNextProbe();
    loading_ = false;
    history_.clear();
    updateUndoActions();
    setModified(false);
    if (interactive)
        rememberRecent(native);
    statusBar()->showMessage(QStringLiteral("Progetto %1 aperto: %2 file, %3 pezzi nel montaggio.")
                                 .arg(QFileInfo(native).fileName())
                                 .arg(project_.clips().size())
                                 .arg(project_.sequence().fragments().size()),
                             8000);
    return true;
}

// Proxy e cartella di lavoro

QString MainWindow::proxyFolder() const
{
    if (projectPath_.isEmpty())
        return QDir::toNativeSeparators(QDir(workFolder()).filePath(QStringLiteral("proxy")));
    const QFileInfo project(projectPath_);
    return QDir::toNativeSeparators(project.dir().filePath(project.completeBaseName() + QStringLiteral(".proxy")));
}

void MainWindow::updateProxies()
{
    proxies_->setFolder(proxyFolder());
    QStringList originals;
    if (!currentPath_.isEmpty())
        originals << currentPath_;  // quello nella Sorgente per primo
    for (const core::Clip& clip : project_.clips())
        originals << qs(clip.path);
    proxies_->request(originals);
    refreshClipList();
}

int MainWindow::previewQuality() const
{
    return proxies_->divisor();
}

void MainWindow::setPreviewQuality(int divisor)
{
    settings().setValue(QStringLiteral("anteprima/qualita"), divisor);
    proxies_->setDivisor(divisor);
    player_->setPreviewQuality(divisor);
    program_->setPreviewQuality(divisor);
    // Con i proxy già pronti si passa subito; gli altri si creano in sottofondo (e si passa quando sono pronti).
    player_->reloadVideo();
    program_->reloadVideo();
    updateProxies();
    statusBar()->showMessage(divisor == 1 ? QStringLiteral("Anteprima dai file originali")
                                          : QStringLiteral("Anteprima dai proxy a 1/%1 della risoluzione (l'esportazione usa "
                                                           "sempre gli originali)")
                                                .arg(divisor),
                             8000);
}

void MainWindow::proxyChanged(const QString& original)
{
    if (currentPath_ == original)
        player_->reloadVideo();
    program_->reloadVideo();  // passa al proxy solo se il pezzo mostrato è di quel file
    refreshClipList();
}

void MainWindow::deleteProxies()
{
    QStringList originals;
    for (const core::Clip& clip : project_.clips())
        originals << qs(clip.path);
    if (originals.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("Elimina i proxy"),
                              QStringLiteral("Cancellare i proxy dei file del progetto da %1?\n\nL'anteprima torna ai file "
                                             "originali; scegliendo di nuovo 1/2, 1/4 o 1/8 i proxy si ricreano.")
                                  .arg(proxies_->folder()))
        != QMessageBox::Yes)
        return;
    // Prima le anteprime tornano agli originali (un file aperto non si può cancellare), poi si cancella.
    setPreviewQuality(1);
    proxies_->removeAll(originals);
    refreshClipList();
    statusBar()->showMessage(QStringLiteral("Proxy eliminati: l'anteprima usa i file originali."), 8000);
}

void MainWindow::chooseWorkFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Cartella di lavoro"), workFolder());
    if (folder.isEmpty())
        return;
    settings().setValue(QStringLiteral("cartellaDiLavoro"), QDir::toNativeSeparators(folder));
    if (projectPath_.isEmpty()) {
        QStringList originals;
        for (const core::Clip& clip : project_.clips())
            originals << qs(clip.path);
        proxies_->moveTo(proxyFolder(), originals);
        player_->reloadVideo();
        program_->reloadVideo();
        proxies_->removeLeftovers();
        updateProxies();
    }
    statusBar()->showMessage(QStringLiteral("Cartella di lavoro: %1").arg(QDir::toNativeSeparators(folder)), 8000);
}

void MainWindow::rememberRecent(const QString& path)
{
    QSettings s = settings();
    QStringList recent = s.value(kRecentProjectsKey).toStringList();
    recent.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; });
    recent.prepend(path);
    while (recent.size() > kRecentProjects)
        recent.removeLast();
    s.setValue(kRecentProjectsKey, recent);
}

void MainWindow::rebuildRecentMenu()
{
    recentMenu_->clear();
    const QStringList recent = settings().value(kRecentProjectsKey).toStringList();
    if (recent.isEmpty()) {
        recentMenu_->addAction(QStringLiteral("(nessuno)"))->setEnabled(false);
        return;
    }
    for (int i = 0; i < recent.size(); ++i) {
        const QString path = recent[i];
        QAction* action = recentMenu_->addAction(QStringLiteral("&%1  %2").arg(i + 1).arg(QFileInfo(path).fileName()), this,
                                                 [this, path] {
                                                     if (!QFileInfo::exists(path)) {
                                                         QMessageBox::warning(this, QStringLiteral("Progetto non trovato"),
                                                                              QStringLiteral("Il progetto %1 non c'è più.").arg(path));
                                                         QSettings s = settings();
                                                         QStringList list = s.value(kRecentProjectsKey).toStringList();
                                                         list.removeAll(path);
                                                         s.setValue(kRecentProjectsKey, list);
                                                         return;
                                                     }
                                                     if (maybeSave())
                                                         openProject(path);
                                                 });
        action->setStatusTip(path);
    }
    recentMenu_->addSeparator();
    recentMenu_->addAction(QStringLiteral("Svuota l'elenco"), this,
                           [] { settings().remove(kRecentProjectsKey); });
}

void MainWindow::showShortcuts()
{
    struct Row
    {
        const char* keys;
        const char* what;
    };
    static const Row rows[] = {
        {"Spazio", "Riproduci / pausa (anteprima attiva: clic su un'anteprima per sceglierla)"},
        {"L / J / K", "Avanti / all'indietro (premuti più volte: 2x, 4x) / pausa"},
        {"← / →  (anche , / .)", "Fotogramma precedente / successivo"},
        {"Maiusc+← / Maiusc+→", "Sorgente: fotogramma chiave precedente / successivo · Montaggio: inizio del pezzo"},
        {"Ctrl+← / Ctrl+→", "Indietro / avanti di 5 secondi"},
        {"Home / Fine", "Inizio / fine"},
        {"Pulsanti sotto le anteprime", "Anche stop, velocità (0,25x - 4x), ripetizione e volume dell'anteprima"},
        {"+ / -  (Ctrl+rotellina)", "Ingrandisci / riduci la linea del tempo dell'anteprima attiva (pulsante Adatta: tutto)"},
        {"Rotellina", "Scorri la linea del tempo ingrandita"},
        {"I / O", "Sorgente: segna IN / OUT · Montaggio: inizio / fine del tratto audio"},
        {"Maiusc+I / Maiusc+O", "Vai a IN / OUT"},
        {"A", "Aggiungi IN-OUT (o il file intero) al montaggio"},
        {"X", "Togli IN-OUT dal montaggio"},
        {"S", "Dividi il pezzo sotto il cursore del montaggio"},
        {"Canc", "Elimina il pezzo selezionato (o il file, se il fuoco è sull'elenco dei file)"},
        {"M / Maiusc+M", "Togli / rimetti l'audio nel tratto selezionato"},
        {"Ctrl+Z", "Annulla"},
        {"Ctrl+Y / Ctrl+Maiusc+Z", "Ripeti"},
        {"Ctrl+O / Ctrl+Maiusc+O", "Apri un file video / aggiungi file al progetto"},
        {"Ctrl+P", "Apri progetto"},
        {"Ctrl+S / Ctrl+Maiusc+S", "Salva progetto / salva con nome"},
        {"Ctrl+E", "Esporta"},
        {"Doppio clic sull'immagine", "Schermo intero dell'anteprima (Esc o doppio clic per uscire)"},
        {"Trascina file da Esplora file", "Aggiungili al progetto (sulla Sorgente: aprili; sulla linea del tempo: anche nel montaggio)"},
        {"F1", "Questo elenco"},
    };
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='4'>");
    for (const Row& row : rows)
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                    .arg(QString::fromUtf8(row.keys).toHtmlEscaped(), QString::fromUtf8(row.what).toHtmlEscaped());
    html += QStringLiteral("</table>");
    QMessageBox box(QMessageBox::NoIcon, QStringLiteral("Scorciatoie da tastiera"), html, QMessageBox::Close, this);
    box.setTextFormat(Qt::RichText);
    box.exec();
}

namespace {

QString describeStream(const core::StreamSignature& s)
{
    if (s.kind == core::StreamKind::Video) {
        return QStringLiteral("%1%2, %3x%4, %5 fotogrammi al secondo, pixel %6")
            .arg(qs(s.codec), s.profile.empty() ? QString() : QStringLiteral(" (%1)").arg(qs(s.profile)))
            .arg(s.width)
            .arg(s.height)
            .arg(qs(core::formatFrameRate(s.frameRate)), qs(s.pixelFormat));
    }
    return QStringLiteral("%1, %2 Hz, %3 canali%4")
        .arg(qs(s.codec))
        .arg(s.sampleRate)
        .arg(s.channels)
        .arg(s.channelLayout.empty() ? QString() : QStringLiteral(" (%1)").arg(qs(s.channelLayout)));
}

}  // namespace

void MainWindow::projectSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Impostazioni del progetto"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(QStringLiteral(
        "Il formato del file esportato. Lo dà un file del progetto: all'inizio quello del primo pezzo messo nel montaggio.\n"
        "I pezzi di file con parametri di codifica diversi (segno P sul blocco) si copiano lo stesso; quelli con codec, "
        "misure o fotogrammi al secondo diversi (segno R) si ricodificano nel formato del progetto all'esportazione."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    auto* source = new QComboBox;
    for (const core::Clip& clip : project_.clips())
        source->addItem(fileNameOf(clip.path), clip.id);
    auto* video = new QLabel;
    auto* audio = new QLabel;
    video->setTextInteractionFlags(Qt::TextSelectableByMouse);
    audio->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Formato preso da:"), source);
    form->addRow(QStringLiteral("Video:"), video);
    form->addRow(QStringLiteral("Audio:"), audio);
    layout->addLayout(form);
    auto* note = new QLabel(QStringLiteral("Per ora codec, risoluzione e fotogrammi al secondo si prendono da un file del "
                                           "progetto: scegli quale."));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // Il formato mostrato: quello del progetto finché non si sceglie un altro file.
    core::ProjectFormat shown = project_.format();
    const auto show = [&] {
        const core::StreamSignature* v = shown.video();
        const core::StreamSignature* a = shown.audio();
        video->setText(v != nullptr ? describeStream(*v) : QStringLiteral("nessuno"));
        audio->setText(a != nullptr ? describeStream(*a) : QStringLiteral("nessuno"));
        if (shown.empty())
            video->setText(QStringLiteral("non ancora deciso (lo darà il primo pezzo messo nel montaggio)"));
    };
    const int current = source->findText(qs(project_.format().sourceName));
    source->setCurrentIndex(current);  // -1 (nessuno) se il formato non viene da un file di adesso
    bool chosen = false;
    connect(source, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
        const core::Clip* clip = project_.find(source->itemData(index).toInt());
        const core::MediaSignature* signature = clip != nullptr ? signatureFor(clip->path) : nullptr;
        if (signature == nullptr)
            return;
        shown = core::formatOf(*signature);
        chosen = true;
        show();
    });
    show();
    if (dialog.exec() != QDialog::Accepted || !chosen)
        return;
    const core::Project before = project_;
    project_.setFormat(shown);
    recordEdit(QStringLiteral("Formato del progetto"), before);
}

void MainWindow::showAbout()
{
    // Quali encoder funzionano su questo PC: la prova (un fotogramma con ognuno) si fa la prima volta che serve, non
    // all'avvio (su un PC lento rallenterebbe tutto il resto), e dura al massimo qualche secondo.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString encoders;
    for (const core::VideoEncoderInfo& info : core::availableVideoEncoders())
        encoders += QStringLiteral("<br>• %1").arg(qs(core::describe(info)).toHtmlEscaped());
    QApplication::restoreOverrideCursor();
    if (encoders.isEmpty())
        encoders = QStringLiteral("<br>• nessuno (la ricodifica non sarà disponibile)");
    QMessageBox::about(this, QStringLiteral("Informazioni su EditorVideo"),
                       QStringLiteral("<b>EditorVideo</b><br>Taglio e unione di video, con ricodifica solo dove serve.<br><br>")
                           + versionSummary().toHtmlEscaped()
                           + QStringLiteral("<br><br>Encoder video funzionanti su questo PC:") + encoders
                           + QStringLiteral("<br>Audio: AAC (FFmpeg)"));
}

QString versionSummary()
{
    const core::FfmpegInfo ffmpeg = core::ffmpegInfo();
    return QStringLiteral("Qt %1  ·  FFmpeg %2 (libavformat %3, libavcodec %4)")
        .arg(QString::fromLatin1(qVersion()), qs(ffmpeg.version),
             qs(ffmpeg.avformat), qs(ffmpeg.avcodec));
}

}  // namespace editor::app
