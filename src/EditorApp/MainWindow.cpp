#include "EditorApp/MainWindow.h"

#include "EditorApp/MainWindowShared.h"
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
#include "EditorCore/MixExport.h"
#include "EditorCore/Proxy.h"
#include "EditorCore/SegmentExport.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorApp/TransitionListPanel.h"
#include "EditorApp/TransitionPreviewWidget.h"
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
#include <QtCore/QUrl>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
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
#include <QtWidgets/QStackedWidget>
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
using namespace detail;


MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , thumbnails_(std::make_unique<ThumbnailCache>(this, [this] { timeline_->thumbnailsChanged(); }))
    , waveforms_(std::make_unique<WaveformStore>(this, [this] { refreshWaveforms(); }))
    , proxies_(std::make_unique<ProxyManager>(this))
{
    setWindowTitle(QStringLiteral("EditorVideo"));
    resize(1500, 900);

    // --- Sinistra: file del progetto, informazioni, fotogrammi chiave
    auto* clipList = new ClipListWidget;
    clipList->pathOf = [this](int clipId) {
        const core::Clip* clip = project_.find(clipId);
        return clip != nullptr ? qs(clip->path) : QString();
    };
    clipList_ = clipList;
    clipList_->setSelectionMode(QAbstractItemView::SingleSelection);
    clipList_->setDragEnabled(true);
    clipList_->setDragDropMode(QAbstractItemView::DragOnly);
    clipList_->setDefaultDropAction(Qt::CopyAction);
    clipList_->setToolTip(QStringLiteral("Clic: mostra il file nella Sorgente · Canc: toglie il file dal progetto · "
                                         "trascina un file sulla linea del tempo per metterlo nel montaggio o su una "
                                         "traccia audio"));
    clipList_->installEventFilter(this);
    connect(clipList_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { showSourcePlayer(); });
    connect(clipList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !updatingList_) {
            setProgramActive(false);
            sourceTabs_->setCurrentIndex(0);  // un file scelto si vede nella scheda Sorgente (non in quella delle scritte)
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
    // La Sorgente mostra il file scelto oppure, scegliendo una transizione nell'elenco, la sua anteprima animata.
    transitionPreview_ = new TransitionPreviewWidget;
    sourceStack_ = new QStackedWidget;
    sourceStack_->addWidget(player_);
    sourceStack_->addWidget(transitionPreview_);
    sourceLayout->addWidget(sourceStack_, 1);

    program_ = new ProgramMonitor(project_);
    program_->onActivated = [this] { setProgramActive(true); };
    program_->onPositionChanged = [this](double seconds) {
        timeline_->setPlayhead(seconds);
        syncQualityMenus();  // il menu del Montaggio mostra la qualità del file del pezzo mostrato
    };
    program_->onCacheRangeChanged = [this](double from, double to) { timeline_->setCacheRange(from, to); };
    program_->onPlaybackStarted = [this] { timeline_->resumeFollowing(); };
    program_->onVideoDoubleClicked = [this] { toggleFullScreen(programDock_); };
    program_->onTextGrabbed = [this](int clipId) { timeline_->setSelectedTextClip(clipId); };
    program_->onTextDragged = [this](int clipId, double x, double y) { moveTextOnScreen(clipId, x, y); };

    // Proxy: le due anteprime leggono il video dal proxy quando è pronto. Ogni file ha la sua qualità: il menu della
    // Sorgente cambia quella del file nella Sorgente, quello del Montaggio quella del file del pezzo mostrato.
    const auto videoSource = [this](const QString& path) { return proxies_->videoSource(path); };
    player_->videoSourceFor = videoSource;
    program_->setVideoSource(videoSource);
    player_->onPreviewQualityChosen = [this](int divisor) {
        if (!currentPath_.isEmpty())
            setPreviewQuality(currentPath_, divisor);
    };
    program_->onPreviewQualityChosen = [this](int divisor) {
        const core::Clip* clip = project_.find(program_->currentClipId());
        if (clip != nullptr)
            setPreviewQuality(qs(clip->path), divisor);
        else
            syncQualityMenus();  // nessun pezzo mostrato (spazio vuoto): il menu torna com'era
    };
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

    // --- In basso: la sequenza
    timeline_ = new SequenceTimeline;
    timeline_->installEventFilter(this);
    timeline_->onSeek = [this](double seconds) {
        setProgramActive(true);
        program_->seekTo(seconds);
    };
    timeline_->onSelect = [this](int) {
        setProgramActive(true);
        showGroupStatus();
    };
    timeline_->groupMembers = [this](SequenceTimeline::Object o) {
        return project_.groupMembers(o);
    };
    timeline_->freeStart = [this](int fragmentId, double desired) {
        return project_.freeSpotFor({core::ObjectRef::Kind::Piece, fragmentId}, 0, desired, pushPieces_);
    };
    timeline_->onMoveTo = [this](int fragmentId, double start) { moveFragment(fragmentId, start); };
    // Bordi dei pezzi (step 16): a passi di un fotogramma dal bordo di partenza, dentro il file, lunghi almeno un
    // fotogramma; durante il trascinamento il Montaggio mostra il fotogramma del nuovo bordo.
    timeline_->trimLimit = [this](int fragmentId, bool atStart, double desired, bool ripple) {
        const core::Fragment* f = project_.sequence().find(fragmentId);
        const core::Clip* clip = f != nullptr ? project_.find(f->clipId) : nullptr;
        if (clip == nullptr)
            return desired;
        const double frame = frameDurationOf(f->clipId);
        const double edge = atStart ? f->start : f->end;
        const double stepped = edge + std::round((desired - edge) / frame) * frame;
        return project_.sequence().trimLimit(fragmentId, atStart ? core::Sequence::Edge::Start : core::Sequence::Edge::End,
                                             stepped, clip->duration, frame, ripple);
    };
    timeline_->onTrimPreview = [this](int fragmentId, bool atStart, double value) {
        const core::Fragment* f = project_.sequence().find(fragmentId);
        if (f == nullptr)
            return;
        // La fine è esclusa: si mostra l'ultimo fotogramma che resta nel pezzo.
        const double shown = atStart ? value : value - frameDurationOf(f->clipId);
        program_->showSourceFrame(f->clipId, shown,
                                  QStringLiteral("%1 del pezzo %2: %3")
                                      .arg(atStart ? QStringLiteral("Inizio") : QStringLiteral("Fine"))
                                      .arg(project_.sequence().indexOf(fragmentId) + 1)
                                      .arg(timeText(value)));
    };
    timeline_->onTrim = [this](int fragmentId, bool atStart, double value, bool ripple) {
        const core::Fragment* f = project_.sequence().find(fragmentId);
        const core::Clip* clip = f != nullptr ? project_.find(f->clipId) : nullptr;
        if (clip == nullptr)
            return;
        const double frame = frameDurationOf(f->clipId);
        const core::Project before = project_;
        if (!project_.sequence().trim(fragmentId, atStart ? core::Sequence::Edge::Start : core::Sequence::Edge::End,
                                      value, clip->duration, frame, ripple)) {
            program_->sequenceChanged();  // torna al punto del montaggio
            return;
        }
        recordEdit(atStart ? QStringLiteral("Sposta inizio del pezzo") : QStringLiteral("Sposta fine del pezzo"), before);
        // Il Montaggio va sul bordo nuovo (la fine: l'ultimo fotogramma del pezzo).
        const core::Sequence& sequence = project_.sequence();
        const int index = sequence.indexOf(fragmentId);
        if (index >= 0) {
            const double start = sequence.startOf(index);
            const double length = sequence.fragments()[static_cast<std::size_t>(index)].length();
            program_->seekTo(atStart ? start : start + std::max(0.0, length - frame));
        }
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
            statusBar()->showMessage(QStringLiteral("Tratto audio %1 - %2: Canc lo toglie dalle clip di questa traccia.")
                                         .arg(timeText(from), timeText(to)),
                                     8000);
    };
    // Tracce audio (step 18).
    timeline_->onAddTrack = [this] { addAudioTrack(); };
    timeline_->onRemoveTrack = [this](int trackId) { removeAudioTrack(trackId); };
    timeline_->onTrackMuted = [this](int trackId, bool muted) { setTrackMuted(trackId, muted); };
    timeline_->onTrackSolo = [this](int trackId, bool solo) { setTrackSolo(trackId, solo); };
    timeline_->onTrackLevel = [this](int trackId, double db) { setTrackLevel(trackId, db); };
    timeline_->onEnvelopeChanged = [this](bool fragment, int id, const std::vector<core::LevelPoint>& points) {
        setEnvelope(fragment, id, points);
    };
    timeline_->onAudioClipSelected = [this](int) {
        setProgramActive(true);
        showGroupStatus();
    };
    timeline_->audioFreePosition = [this](int clipId, int trackId, double desired) {
        return project_.freeSpotFor({core::ObjectRef::Kind::AudioClip, clipId}, trackId, desired, pushPieces_);
    };
    timeline_->onAudioClipMoveTo = [this](int clipId, int trackId, double position) { moveAudioClip(clipId, trackId, position); };
    timeline_->audioTrimLimit = [this](int clipId, bool atStart, double desired, bool ripple) {
        return project_.audioTracks().trimLimit(clipId, atStart ? core::Sequence::Edge::Start : core::Sequence::Edge::End,
                                                desired, kMinAudioClip, ripple);
    };
    timeline_->onAudioTrim = [this](int clipId, bool atStart, double value, bool ripple) {
        trimAudioClip(clipId, atStart, value, ripple);
    };
    timeline_->onAudioFilesDropped = [this](const QStringList& paths, int trackId, double seconds) {
        addAudioFiles(paths, trackId, seconds);
    };
    // Scritte (step T3).
    timeline_->onTextClipSelected = [this](int) {
        setProgramActive(true);
        showGroupStatus();
        refreshTextPanel();
    };
    timeline_->textFreePosition = [this](int clipId, int layerId, double desired) {
        return project_.freeSpotFor({core::ObjectRef::Kind::TextClip, clipId}, layerId, desired, pushPieces_);
    };
    timeline_->onTextClipMoveTo = [this](int clipId, int layerId, double position) { moveTextClip(clipId, layerId, position); };
    timeline_->textTrimLimit = [this](int clipId, bool atStart, double desired) {
        return project_.textTracks().edgeLimit(clipId, atStart, desired);
    };
    timeline_->onTextTrim = [this](int clipId, bool atStart, double value) { trimTextClip(clipId, atStart, value); };
    timeline_->onTextFades = [this](int clipId, double fadeIn, double fadeOut) { fadeTextClip(clipId, fadeIn, fadeOut); };
    timeline_->onTextActivate = [this](int clipId) { editTextClip(clipId); };
    // Transizioni (step X5).
    timeline_->canDropTransition = [this](int beforeFragment) { return canDropTransition(beforeFragment); };
    timeline_->onTransitionDropped = [this](const QString& type, int beforeFragment) { addTransition(type, beforeFragment); };
    timeline_->onTransitionDropRefused = [this](int beforeFragment) { refuseTransition(beforeFragment); };
    timeline_->onTransitionSelected = [this](int) { setProgramActive(true); };
    timeline_->transitionLimit = [this](int id, double desired) { return clampTransitionDuration(id, desired); };
    timeline_->onTransitionDuration = [this](int id, double duration) { changeTransitionDuration(id, duration); };
    timeline_->onTransitionActivate = [this](int id) { editTransition(id); };
    sequenceLabel_ = new QLabel;
    // L'etichetta resta alta quanto il suo testo: lo spazio in più va tutto alla linea del tempo (stretch sotto).
    sequenceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
    auto* sequencePanel = new QWidget;
    auto* sequenceLayout = new QVBoxLayout(sequencePanel);
    sequenceLayout->setContentsMargins(0, 0, 0, 0);
    sequenceLayout->addWidget(sequenceLabel_, 0);
    sequenceLayout->addWidget(timeline_, 1);
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
    // Il blocco in alto a sinistra ha due schede: i file del progetto e l'elenco delle transizioni.
    transitionPanel_ = new TransitionListPanel;
    transitionPanel_->onSelected = [this](const core::Transition* transition) {
        if (transition == nullptr)
            return;
        transitionPreview_->setTransition(transition);
        sourceStack_->setCurrentWidget(transitionPreview_);
        sourceTabs_->setCurrentIndex(0);
    };
    leftTabs_ = new QTabWidget;
    leftTabs_->setDocumentMode(true);
    leftTabs_->addTab(leftPanel, QStringLiteral("File del progetto"));
    leftTabs_->addTab(transitionPanel_, QStringLiteral("Transizioni"));
    filesDock_ = makeDock(QStringLiteral("File del progetto"), QStringLiteral("pannelloFile"), leftTabs_);
    // Il pannello Sorgente ha una scheda in più, "Scritta": l'editor della scritta scelta (le modifiche valgono subito e si
    // vedono nel Montaggio, che resta a fianco).
    textPanel_ = new TextEditorPanel;
    textPanel_->onEdited = [this](const core::TextClip& edited) { applyTextEdit(edited, nullptr, true); };
    textPanel_->onStylesChanged = [this](const std::vector<core::NamedTextStyle>& styles) {
        const core::Project before = project_;
        keepTextStyles(styles);
        recordEdit(QStringLiteral("Cambia stili delle scritte"), before);
    };
    sourceTabs_ = new QTabWidget;
    sourceTabs_->setDocumentMode(true);
    sourceTabs_->addTab(sourcePanel, QStringLiteral("Sorgente"));
    sourceTabs_->addTab(textPanel_, QStringLiteral("Scritta"));
    sourceDock_ = makeDock(QStringLiteral("Sorgente"), QStringLiteral("pannelloSorgente"), sourceTabs_);
    programDock_ = makeDock(QStringLiteral("Montaggio"), QStringLiteral("pannelloMontaggio"), program_);
    timelineDock_ = makeDock(QStringLiteral("Linea del tempo del montaggio"), QStringLiteral("pannelloLineaDelTempo"),
                             sequencePanel);
    // Spiegazioni, nel suggerimento sul titolo del pannello (vedi eventFilter).
    filesDock_->setProperty("spiegazione", QStringLiteral("I file del progetto: clic per mostrarne uno nella Sorgente, "
                                                          "Canc per toglierlo; si aggiungono anche trascinandoli qui. "
                                                          "Nella scheda Transizioni, le transizioni disponibili"));
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
    if (core::isAudioOnly(clip->path)) {
        addSourceAudioToTrack(*clip, in, out);
        return;
    }

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
    // Un oggetto di un gruppo: si dividono tutti quelli del gruppo che il cursore attraversa, e le seconde parti formano
    // un gruppo nuovo.
    core::ObjectRef anchor;
    if (const int text = timeline_->selectedTextClip(); text != 0)
        anchor = {core::ObjectRef::Kind::TextClip, text};
    else if (const int clip = timeline_->selectedAudioClip(); clip != 0)
        anchor = {core::ObjectRef::Kind::AudioClip, clip};
    else if (const int index = project_.sequence().indexAt(at); index >= 0)
        anchor = {core::ObjectRef::Kind::Piece, project_.sequence().fragments()[static_cast<std::size_t>(index)].id};
    if (anchor.id != 0 && project_.groupMoverCount(anchor) > 1) {
        const std::vector<core::ObjectRef> parts = project_.splitGroup(anchor, at);
        if (parts.empty()) {
            statusBar()->showMessage(QStringLiteral("Qui non si può dividere: il cursore non attraversa nessun oggetto del gruppo."), 8000);
            return;
        }
        for (const core::ObjectRef& part : parts) {
            if (part.kind != anchor.kind)
                continue;
            if (part.kind == core::ObjectRef::Kind::AudioClip)
                timeline_->setSelectedAudioClip(part.id);
            else if (part.kind == core::ObjectRef::Kind::TextClip)
                timeline_->setSelectedTextClip(part.id);
            else
                timeline_->setSelected(part.id);
        }
        recordEdit(QStringLiteral("Dividi gruppo"), before);
        statusBar()->showMessage(QStringLiteral("Gruppo diviso a %1: %2 oggetti divisi, le parti dopo sono un gruppo nuovo.")
                                     .arg(timeText(at))
                                     .arg(parts.size()),
                                 8000);
        return;
    }
    // Scritta selezionata: si divide al cursore.
    if (const int text = timeline_->selectedTextClip(); text != 0) {
        if (const int id = project_.textTracks().split(text, at); id != 0) {
            timeline_->setSelectedTextClip(id);
            recordEdit(QStringLiteral("Dividi scritta"), before);
            statusBar()->showMessage(QStringLiteral("Scritta divisa a %1.").arg(timeText(at)), 8000);
        } else {
            statusBar()->showMessage(QStringLiteral("Qui non si può dividere: porta il cursore dentro la scritta, non su un suo bordo."), 8000);
        }
        return;
    }
    // Clip audio selezionata: si divide la clip della sua traccia sotto il cursore.
    if (const int clip = timeline_->selectedAudioClip(); clip != 0) {
        core::AudioTracks& audio = project_.audioTracks();
        const int track = audio.trackOf(clip);
        if (const int id = audio.split(track, at); id != 0) {
            timeline_->setSelectedAudioClip(id);
            recordEdit(QStringLiteral("Dividi clip audio"), before);
            statusBar()->showMessage(QStringLiteral("Clip audio divisa a %1.").arg(timeText(at)), 8000);
        } else {
            statusBar()->showMessage(QStringLiteral("Qui non si può dividere: porta il cursore dentro una clip della traccia %1.")
                                         .arg(qs(audio.trackName(track))),
                                     8000);
        }
        return;
    }
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
    if (const int transition = timeline_->selectedTransition(); transition != 0) {
        removeTransition(transition);
        return;
    }
    if (timeline_->selectedObjects().size() >= 2) {
        // Più oggetti scelti con Ctrl+clic: si tolgono solo quelli (gli altri del gruppo restano).
        if (project_.removeObjects(selectedRefs()) > 0) {
            timeline_->setSelected(0);
            recordEdit(QStringLiteral("Elimina oggetti"), before);
        }
        return;
    }
    if (const int text = timeline_->selectedTextClip(); text != 0) {
        // Scritta selezionata: si toglie.
        if (project_.textTracks().remove(text)) {
            timeline_->setSelectedTextClip(0);
            recordEdit(QStringLiteral("Togli scritta"), before);
        }
        return;
    }
    if (const int clip = timeline_->selectedAudioClip(); clip != 0) {
        // Clip audio selezionata: si toglie (lì resta silenzio).
        if (project_.audioTracks().remove(clip)) {
            timeline_->setSelectedAudioClip(0);
            recordEdit(QStringLiteral("Togli clip audio"), before);
        }
        return;
    }
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
        return;
    }
    // Tratto selezionato su una traccia aggiunta: si toglie quel tratto dalle clip (lì resta silenzio).
    if (timeline_->hasAudioSelection()) {
        const int track = timeline_->audioSelectionTrack();
        if (track == core::AudioTracks::kOriginalTrack)
            return;
        const double from = timeline_->audioSelectionFrom();
        const double to = timeline_->audioSelectionTo();
        if (project_.audioTracks().removeRange(track, from, to)) {
            timeline_->setAudioSelection(0.0, 0.0);
            recordEdit(QStringLiteral("Togli tratto audio"), before);
            statusBar()->showMessage(QStringLiteral("Tolto da %1 a %2 sulla traccia %3.")
                                         .arg(timeText(from), timeText(to), qs(project_.audioTracks().trackName(track))),
                                     8000);
        }
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

void MainWindow::recordMixEdit(const QString& label, const core::Project& before)
{
    // Livello, muto e solo si sentono subito, anche suonando: la riproduzione non si ferma.
    recordHistory(label, before);
    if (!loading_)
        setModified(true);
    refreshTimeline();
    program_->audioMixChanged();
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
    requestAudioWaveforms();
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
    // Le transizioni la cui giunzione non c'è più (pezzo tolto, spostato, accorciato) si tolgono o si accorciano.
    if (!loading_ && project_.pruneTransitions() > 0)
        statusBar()->showMessage(QStringLiteral("Una transizione non ci stava più ed è stata tolta o accorciata (Annulla per "
                                                "rimetterla)."),
                                 8000);
    if (!loading_)
        setModified(true);
    ensureProjectFormat();
    refreshTimeline();
    program_->sequenceChanged();
    updateSourceBands();
    updateProxies();
    refreshClipList();
    updateExportAction();
    refreshTextPanel();
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
    timeline_->setWaveforms(
        [this](int clipId) {
            const core::Clip* clip = project_.find(clipId);
            return clip != nullptr ? waveforms_->get(clip->path) : nullptr;
        },
        [this](const QString& path) { return waveforms_->get(path.toStdString()); });
}

std::optional<double> MainWindow::knownFrameDurationOf(int clipId) const
{
    const auto info = clipInfo_.find(clipId);
    if (info != clipInfo_.end()) {
        for (const core::StreamInfo& st : info->second.streams) {
            if (st.kind == core::StreamKind::Video && !st.attachedPicture && st.frameRate > 0)
                return 1.0 / st.frameRate;
        }
    }
    return std::nullopt;
}

double MainWindow::frameDurationOf(int clipId) const
{
    return knownFrameDurationOf(clipId).value_or(1.0 / 30);  // non si sa ancora (o solo audio): un trentesimo di secondo
}

void MainWindow::refreshTimeline()
{
    const core::Sequence& sequence = project_.sequence();
    std::vector<SequenceTimeline::Block> blocks;
    const auto replacement = project_.audioReplacement();
    const bool withAudio = !replacement.has_value();
    // Segno e spiegazione dipendono dal file, non dal pezzo: si calcolano una volta per file.
    struct Fit
    {
        int fit = 0;
        QString note;
    };
    std::map<int, Fit> fits;
    for (const core::Fragment& f : sequence.fragments()) {
        const core::Clip* clip = project_.find(f.clipId);
        blocks.push_back({f.id, project_.indexOf(f.clipId), clip ? fileNameOf(clip->path) : QString(), f.length(), f.start,
                          f.clipId, clip ? waveforms_->get(clip->path) : nullptr, f.gap});
        if (clip != nullptr)
            blocks.back().proxyDivisor = proxies_->sourceDivisor(qs(clip->path));
        blocks.back().audioDetached = f.detached;
        if (withAudio)
            blocks.back().envelope = f.envelope;
        // Come entra nel file esportato, rispetto al formato del progetto: segno sul blocco e spiegazione.
        const core::MediaSignature* signature = clip != nullptr ? signatureFor(clip->path) : nullptr;
        if (signature != nullptr && !project_.format().empty()) {
            auto known = fits.find(f.clipId);
            if (known == fits.end()) {
                const core::ClipFitResult fit = core::classifyClip(*signature, project_.format(), withAudio);
                Fit entry;
                entry.fit = static_cast<int>(fit.fit);
                QString reasons;
                for (const std::string& reason : fit.reasons)
                    reasons += QStringLiteral("\n• ") + qs(reason);
                if (fit.fit == core::ClipFit::CopyWithParameters)
                    entry.note = QStringLiteral("Si copia senza ricodifica, con i suoi parametri di codifica ripetuti nel "
                                                "file esportato (diversi da quelli del formato del progetto):")
                               + reasons;
                else if (fit.fit == core::ClipFit::Reencode)
                    entry.note = QStringLiteral("Diverso dal formato del progetto (%1): all'esportazione sarà ricodificato "
                                                "in quel formato.")
                                     .arg(qs(project_.format().sourceName))
                               + reasons;
                known = fits.emplace(f.clipId, std::move(entry)).first;
            }
            blocks.back().fit = known->second.fit;
            blocks.back().fitNote = known->second.note;
        }
    }
    timeline_->setBlocks(std::move(blocks));
    // Zoom massimo: 12 pixel per fotogramma del primo file con un video (30 fps se non si sa ancora).
    double frame = 1.0 / 30;
    for (const core::Fragment& f : sequence.fragments()) {
        if (const auto known = knownFrameDurationOf(f.clipId)) {
            frame = *known;
            break;
        }
    }
    timeline_->setFrameDuration(frame);
    const std::vector<core::TimeRange> muted = sequence.mutedRanges(false);  // senza i pezzi con l'audio scollegato
    timeline_->setMuted(muted);
    // Corsie audio: A1 e le tracce aggiunte con le loro clip.
    const core::AudioTracks& audio = project_.audioTracks();
    std::vector<SequenceTimeline::AudioLane> lanes;
    const auto laneOf = [&](int trackId) {
        const core::AudioMix& mix = audio.mix(trackId);
        return SequenceTimeline::AudioLane{trackId, qs(audio.trackName(trackId)), mix.levelDb, mix.muted, mix.solo,
                                           audio.audible(trackId), {}};
    };
    lanes.push_back(laneOf(core::AudioTracks::kOriginalTrack));
    int audioClips = 0;
    for (const core::AudioTrack& track : audio.tracks()) {
        // Le clip libere di A1 stanno nella corsia A1 (la prima), le altre tracce hanno la loro corsia.
        if (!track.host)
            lanes.push_back(laneOf(track.id));
        SequenceTimeline::AudioLane& lane = track.host ? lanes.front() : lanes.back();
        for (const core::AudioClip& c : track.clips) {
            lane.clips.push_back({c.id, qs(c.path), fileNameOf(c.path), colorIndexFor(c.path), c.position, c.length(),
                                          c.start, waveforms_->get(c.path), c.envelope});
            ++audioClips;
        }
    }
    timeline_->setAudioLanes(std::move(lanes));
    // Livelli di scritte, dal più basso al più alto.
    std::vector<SequenceTimeline::TextLane> textLanes;
    for (const core::TextLayer& layer : project_.textTracks().layers()) {
        SequenceTimeline::TextLane lane;
        lane.layerId = layer.id;
        lane.name = qs(project_.textTracks().layerName(layer.id));
        for (const core::TextClip& c : layer.clips) {
            QString label = QString::fromUtf8(c.text.c_str());
            const qsizetype newline = label.indexOf(QLatin1Char('\n'));
            if (newline >= 0)
                label = label.left(newline) + QStringLiteral("…");
            lane.clips.push_back({c.id, label, c.position, c.length, c.fadeIn, c.fadeOut});
        }
        textLanes.push_back(std::move(lane));
    }
    timeline_->setTextLanes(std::move(textLanes));
    refreshTransitions();
    if (removeTextLayerAction_ != nullptr)
        removeTextLayerAction_->setEnabled(!project_.textTracks().layers().empty());
    if (editTextAction_ != nullptr)
        editTextAction_->setEnabled(!project_.textTracks().layers().empty());
    if (timeline_->hasAudioSelection() && timeline_->audioSelectionFrom() >= std::max(sequence.totalLength(), audio.endTime()))
        timeline_->setAudioSelection(0.0, 0.0);
    if (originalAudioAction_ != nullptr)
        originalAudioAction_->setEnabled(replacement.has_value());
    if (removeTrackAction_ != nullptr)
        removeTrackAction_->setEnabled(audio.addedTrackCount() > 0);

    QString text = sequence.empty() ? QStringLiteral("<b>Montaggio</b>: vuoto")
                                    : QStringLiteral("<b>Montaggio</b>: %1 pezzi  ·  durata <b>%2</b>")
                                          .arg(sequence.fragments().size())
                                          .arg(timeText(sequence.totalLength()));
    if (const int gaps = sequence.gapCount(); gaps > 0)
        text += gaps == 1 ? QStringLiteral("  ·  1 spazio vuoto") : QStringLiteral("  ·  %1 spazi vuoti").arg(gaps);
    if (replacement)
        text += QStringLiteral("  ·  audio: <b>%1</b> (al posto dell'originale)").arg(fileNameOf(replacement->path).toHtmlEscaped());
    else if (audioClips > 0)
        text += QStringLiteral("  ·  %1 clip audio su %2 tracce (audio mixato in esportazione)")
                    .arg(audioClips)
                    .arg(audio.addedTrackCount() + 1);
    if (!muted.empty())
        text += QStringLiteral("  ·  %1 tratti senza audio").arg(muted.size());
    sequenceLabel_->setText(text);
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
                                          Qt::Key_S, Qt::Key_J, Qt::Key_K, Qt::Key_L, Qt::Key_Z,
                                          Qt::Key_G, Qt::Key_U, Qt::Key_Y, Qt::Key_Plus, Qt::Key_Minus, Qt::Key_Equal, Qt::Key_Delete};
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
        utf8.push_back(nativeAbsoluteUtf8(path));
    core::DropPlan plan = core::planDrop(utf8, project_, core::inspectMedia);

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
    // Sulla traccia video del montaggio non vanno i file solo audio: vanno su una traccia audio, nel punto di rilascio.
    QStringList audioOnly;
    if (target == DropTarget::Sequence) {
        std::vector<core::DroppedMedia> video;
        for (const core::DroppedMedia& media : plan.media) {
            if (core::isAudioOnly(media.path))
                audioOnly.append(qs(media.path));
            else
                video.push_back(media);
        }
        plan.media = std::move(video);
    }
    if (!audioOnly.isEmpty()) {
        const core::Sequence& sequence = project_.sequence();
        const int index = std::max(0, sequenceIndex);
        const double seconds = index < static_cast<int>(sequence.fragments().size()) ? sequence.startOf(index) : sequence.totalLength();
        addAudioFiles(audioOnly, audioTrackForNewClips(), seconds);
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
        analyzeNewFile(ids[i], plan.media[i].path);
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
    QAction* inAction = segmentMenu->addAction(QStringLiteral("Segna &IN (inizio)"), this, [this] { player_->markIn(); });
    inAction->setShortcut(QKeySequence(Qt::Key_I));
    inAction->setStatusTip(QStringLiteral("Inizio del pezzo nella Sorgente"));
    QAction* outAction = segmentMenu->addAction(QStringLiteral("Segna &OUT (fine)"), this, [this] { player_->markOut(); });
    outAction->setShortcut(QKeySequence(Qt::Key_O));
    outAction->setStatusTip(QStringLiteral("Fine del pezzo nella Sorgente"));
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
    // Una spunta verde a sinistra quando è acceso (disegnata qui: si vede in qualsiasi stile dei menu).
    const auto tickIcon = [](bool on) {
        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);
        if (on) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(40, 160, 70), 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF points[] = {{6, 17}, {13, 24}, {26, 8}};
            painter.drawPolyline(points, 3);
        }
        pixmap.setDevicePixelRatio(2.0);
        return QIcon(pixmap);
    };
    QAction* push = editMenu->addAction(QStringLiteral("S&pingi i pezzi successivi"));
    pushPieces_ = settings().value(QStringLiteral("montaggio/spingiPezzi"), true).toBool();
    push->setIcon(tickIcon(pushPieces_));
    push->setIconVisibleInMenu(true);
    push->setStatusTip(QStringLiteral("Con la spunta: spostando un pezzo verso destra spinge quelli che segue. Senza: si ferma contro il pezzo successivo, o lo supera"));
    connect(push, &QAction::triggered, this, [this, push, tickIcon] {
        pushPieces_ = !pushPieces_;
        push->setIcon(tickIcon(pushPieces_));
        QSettings s = settings();
        s.setValue(QStringLiteral("montaggio/spingiPezzi"), pushPieces_);
        statusBar()->showMessage(pushPieces_ ? QStringLiteral("Spinta dei pezzi successivi: attiva.")
                                             : QStringLiteral("Spinta dei pezzi successivi: spenta."),
                                 6000);
    });
    editMenu->addSeparator();
    QAction* group = editMenu->addAction(QStringLiteral("Crea &gruppo"), this, [this] { groupSelection(); });
    group->setShortcut(QKeySequence(Qt::Key_G));
    group->setStatusTip(QStringLiteral("Gli oggetti scelti con Ctrl+clic si muovono insieme"));
    QAction* ungroup = editMenu->addAction(QStringLiteral("Sc&ioglie il gruppo"), this, [this] { ungroupSelection(); });
    ungroup->setShortcut(QKeySequence(Qt::Key_U));
    ungroup->setStatusTip(QStringLiteral("Scioglie il gruppo dell'oggetto scelto; un video con il suo audio in gruppo: scollega l'audio"));
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("&Svuota il montaggio"), this, [this] { clearSequence(); });
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("&Impostazioni del progetto..."), this, [this] { projectSettingsDialog(); })
        ->setStatusTip(QStringLiteral("Il formato del file esportato: codec, risoluzione, fotogrammi al secondo, audio"));

    QMenu* audioMenu = menuBar()->addMenu(QStringLiteral("A&udio"));
    QAction* replace = audioMenu->addAction(QStringLiteral("&Sostituisci l'audio..."), this, &MainWindow::replaceAudioDialog);
    replace->setStatusTip(QStringLiteral("Un file audio (es. MP3 o M4A) al posto dell'audio originale di tutto il montaggio"));
    originalAudioAction_ = audioMenu->addAction(QStringLiteral("Usa l'audio &originale"), this, [this] { restoreOriginalAudio(); });
    originalAudioAction_->setEnabled(false);
    audioMenu->addSeparator();
    audioMenu->addAction(QStringLiteral("&Aggiungi traccia audio"), this, [this] { addAudioTrack(); })
        ->setStatusTip(QStringLiteral("Una traccia A2, A3... sotto A1, per mettere file audio dove si vuole"));
    removeTrackAction_ = audioMenu->addAction(QStringLiteral("Togli traccia audio"), this, [this] {
        // Quella scelta sulla linea del tempo, altrimenti l'ultima.
        const core::AudioTracks& audio = project_.audioTracks();
        if (audio.addedTrackCount() == 0)
            return;
        const int selected = timeline_->selectedTrack();
        removeAudioTrack(selected != core::AudioTracks::kOriginalTrack && audio.track(selected) != nullptr ? selected : audio.lastAddedTrack());
    });
    removeTrackAction_->setEnabled(false);
    audioMenu->addAction(QStringLiteral("Aggiungi &file audio al cursore..."), this, [this] { addAudioFilesDialog(); })
        ->setStatusTip(QStringLiteral("Sulla traccia scelta (o su una nuova), dal punto del cursore del montaggio"));
    audioMenu->addSeparator();
    audioMenu->addAction(QStringLiteral("Ricolle&ga l'audio al pezzo"), this, [this] { relinkAudio(); })
        ->setStatusTip(QStringLiteral("Se la clip è ancora allineata al pezzo selezionato, l'audio torna a essere quello del pezzo"));

    QMenu* textMenu = menuBar()->addMenu(QStringLiteral("Sc&ritte"));
    QAction* newText = textMenu->addAction(QStringLiteral("&Nuova scritta al cursore"), this, [this] { addTextClip(); });
    newText->setShortcut(QKeySequence(Qt::Key_T));
    newText->setStatusTip(QStringLiteral("Una scritta di 5 secondi sul livello scelto (o dove c'è posto), dal cursore del montaggio"));
    editTextAction_ = textMenu->addAction(QStringLiteral("&Modifica scritta"), this, [this] { editTextClip(timeline_->selectedTextClip()); });
    editTextAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    editTextAction_->setStatusTip(QStringLiteral("Testo, font, colore, contorno, ombra, sfondo e posizione della scritta scelta (anche con doppio clic)"));
    editTextAction_->setEnabled(false);
    textMenu->addSeparator();
    textMenu->addAction(QStringLiteral("&Aggiungi livello di scritte"), this, [this] { addTextLayer(); })
        ->setStatusTip(QStringLiteral("Un livello T1, T2... sopra il video: un livello più alto copre quelli sotto"));
    removeTextLayerAction_ = textMenu->addAction(QStringLiteral("&Togli livello di scritte"), this, [this] { removeSelectedTextLayer(); });
    removeTextLayerAction_->setStatusTip(QStringLiteral("Il livello scelto sulla linea del tempo, altrimenti l'ultimo, con le sue scritte"));
    removeTextLayerAction_->setEnabled(false);

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
    proxies_->clearDivisors();
    proxies_->setFolder(proxyFolder());
    syncQualityMenus();
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
        const std::string utf8 = nativeAbsoluteUtf8(path);
        const int id = project_.add(utf8);
        if (first == 0)
            first = id;
        analyzeNewFile(id, utf8);
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
    // I fotogrammi chiave letti si salvano nella cartella di lavoro, per riaprire lo stesso file (o il progetto) senza
    // rileggerli.
    const std::string cacheFolder =
        QDir::toNativeSeparators(QDir(workFolder()).filePath(QStringLiteral("keyframes"))).toStdString();
    probeThread_ = QThread::create([utf8Path, cacheFolder, outcome, cancel = &cancelProbe_] {
        try {
            outcome->info = core::probeFile(utf8Path, cancel, cacheFolder);
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
                // Le transizioni di un progetto appena aperto si vedono (e si controllano i margini) solo quando i file
                // sono analizzati: finché non si sa quanto durano non c'è una giunzione valida.
                if (!project_.transitions().empty()) {
                    if (project_.allAnalyzed())
                        project_.pruneTransitions();
                    refreshTransitions();
                }
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

void MainWindow::showSourcePlayer()
{
    sourceStack_->setCurrentWidget(player_);
}

void MainWindow::selectClip(int id)
{
    const core::Clip* clip = project_.find(id);
    if (clip == nullptr)
        return;
    showSourcePlayer();  // un file scelto sostituisce l'anteprima di una transizione
    if (id == currentClipId_)
        return;

    currentClipId_ = id;
    currentPath_ = qs(clip->path);
    syncQualityMenus();
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
        if (const int divisor = proxies_->divisor(qs(clip.path)); divisor > 1) {
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
    for (const core::Clip& clip : project_.clips()) {
        if (const int divisor = proxies_->divisor(qs(clip.path)); divisor > 1)
            view.proxyDivisors[clip.id] = divisor;
    }
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
        {"I / O", "Sorgente: segna IN / OUT"},
        {"Maiusc+I / Maiusc+O", "Vai a IN / OUT"},
        {"A", "Aggiungi IN-OUT (o il file intero) al montaggio"},
        {"X", "Togli IN-OUT dal montaggio"},
        {"S", "Dividi il pezzo (o la scritta, o la clip audio) sotto il cursore del montaggio"},
        {"T", "Nuova scritta al cursore del montaggio (si apre l'editor)"},
        {"Ctrl+T / doppio clic", "Modifica la scritta scelta"},
        {"Canc", "Elimina l'oggetto selezionato (o il file, se il fuoco è sull'elenco dei file)"},
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
