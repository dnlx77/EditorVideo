#include "EditorApp/MainWindow.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorCore/LibraryInfo.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/SegmentExport.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <iterator>
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

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("EditorVideo"));
    resize(1300, 800);

    placeholder_ = new QLabel(QStringLiteral("Apri un file video da File > Apri..."));
    placeholder_->setAlignment(Qt::AlignCenter);

    infoTree_ = new QTreeWidget;
    infoTree_->setColumnCount(2);
    infoTree_->setHeaderLabels({QStringLiteral("Proprietà"), QStringLiteral("Valore")});
    infoTree_->setAlternatingRowColors(true);
    infoTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    infoTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    // A destra l'elenco dei fotogrammi chiave: sono i punti in cui si potrà tagliare senza ricodifica.
    keyframeTitle_ = new QLabel;
    keyframeList_ = new QTreeWidget;
    keyframeList_->setColumnCount(2);
    keyframeList_->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Tempo")});
    keyframeList_->setRootIsDecorated(false);
    keyframeList_->setUniformRowHeights(true);  // veloce anche con migliaia di righe
    keyframeList_->setAlternatingRowColors(true);
    keyframeList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    keyframeList_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    auto* keyframePanel = new QWidget;
    auto* keyframeLayout = new QVBoxLayout(keyframePanel);
    keyframeLayout->setContentsMargins(0, 0, 0, 0);
    keyframeLayout->addWidget(keyframeTitle_);
    keyframeLayout->addWidget(keyframeList_);

    // Gli elenchi userebbero Spazio e frecce per la selezione: li lasciamo ai comandi di riproduzione.
    infoTree_->installEventFilter(this);
    keyframeList_->installEventFilter(this);

    // Un clic su un fotogramma chiave porta l'anteprima in quel punto.
    connect(keyframeList_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        player_->seekTo(item->data(1, Qt::UserRole).toDouble());
    });

    player_ = new PlayerWidget;
    player_->onSegmentChanged = [this] {
        // I tratti del file mostrato si salvano subito nel progetto.
        storeCurrentRanges();
        refreshClipList();
        updateExportAction();
    };

    // File del progetto: si uniscono nell'ordine dell'elenco, che si cambia trascinando.
    clipList_ = new QListWidget;
    clipList_->setDragDropMode(QAbstractItemView::InternalMove);
    clipList_->setDefaultDropAction(Qt::MoveAction);
    clipList_->setSelectionMode(QAbstractItemView::SingleSelection);
    clipList_->setToolTip(QStringLiteral("Clic per modificare un file; trascina per cambiare l'ordine; Canc per toglierlo"));
    clipList_->installEventFilter(this);
    connect(clipList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !updatingList_)
            selectClip(item->data(Qt::UserRole).toInt());
    });
    // Dopo il trascinamento l'elenco viene ricostruito: lo si fa al giro successivo, non dentro la notifica del modello.
    connect(clipList_->model(), &QAbstractItemModel::rowsMoved, this,
            [this] { QTimer::singleShot(0, this, [this] { syncOrderFromList(); }); });
    projectLabel_ = new QLabel;
    projectLabel_->setWordWrap(true);

    auto* clipPanel = new QWidget;
    auto* clipLayout = new QVBoxLayout(clipPanel);
    clipLayout->setContentsMargins(0, 0, 0, 0);
    clipLayout->addWidget(new QLabel(QStringLiteral("<b>File del progetto</b> (si uniscono in quest'ordine)")));
    clipLayout->addWidget(clipList_);
    clipLayout->addWidget(projectLabel_);

    // Anteprima al centro; a destra i file del progetto, le informazioni sul file mostrato e i fotogrammi chiave.
    auto* sidePanel = new QSplitter(Qt::Vertical);
    sidePanel->addWidget(clipPanel);
    sidePanel->addWidget(infoTree_);
    sidePanel->addWidget(keyframePanel);
    sidePanel->setStretchFactor(0, 1);
    sidePanel->setStretchFactor(1, 2);
    sidePanel->setStretchFactor(2, 1);

    auto* splitter = new QSplitter;
    splitter->addWidget(player_);
    splitter->addWidget(sidePanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 380});

    pages_ = new QStackedWidget(this);
    pages_->addWidget(placeholder_);
    pages_->addWidget(splitter);
    setCentralWidget(pages_);

    createMenus();
    statusBar()->showMessage(QStringLiteral("Pronto  ·  ") + versionSummary());
}

MainWindow::~MainWindow()
{
    // Se un'analisi è ancora in corso la interrompiamo e aspettiamo che il thread finisca.
    if (probeThread_ != nullptr) {
        cancelProbe_ = true;
        probeThread_->wait();
    }
    // Lo stesso per un'esportazione: viene annullata e il file a metà cancellato.
    if (exportThread_ != nullptr) {
        cancelExport_ = true;
        exportThread_->wait();
    }
}

std::vector<core::ClipRanges> MainWindow::exportPlan() const
{
    if (project_.empty() || !project_.allAnalyzed())
        return {};
    if (project_.clips().size() > 1)
        return project_.exportPlan();  // ogni file con i suoi tratti, o intero

    // Un solo file: i suoi tratti, oppure il solo IN-OUT (come allo step 4). Il file intero no: sarebbe una copia.
    const core::Clip& clip = project_.clips().front();
    if (!clip.segments.empty())
        return {{clip.path, clip.segments.ranges()}};
    const core::CutSegment& segment = player_->segment();
    if (segment.hasIn() && segment.hasOut())
        return {{clip.path, {{segment.in(), segment.out()}}}};
    return {};
}

double MainWindow::exportLength() const
{
    double total = 0.0;
    for (const core::ClipRanges& clip : exportPlan()) {
        for (const core::TimeRange& r : clip.ranges)
            total += r.length();
    }
    return total;
}

void MainWindow::updateExportAction()
{
    if (exportAction_ == nullptr)
        return;
    const auto plan = exportPlan();
    std::size_t ranges = 0;
    for (const auto& clip : plan)
        ranges += clip.ranges.size();
    exportAction_->setEnabled(!plan.empty() && exportThread_ == nullptr);
    if (project_.clips().size() > 1)
        exportAction_->setText(QStringLiteral("&Esporta %1 file uniti...").arg(project_.clips().size()));
    else
        exportAction_->setText(ranges > 1 ? QStringLiteral("&Esporta %1 tratti uniti...").arg(ranges)
                                          : QStringLiteral("&Esporta segmento..."));
    QString tip;
    if (!project_.allAnalyzed())
        tip = QStringLiteral("Attendi la fine dell'analisi dei file");
    else if (plan.empty())
        tip = QStringLiteral("Segna IN e OUT (tasti I e O), aggiungi dei tratti (tasto A) o aggiungi altri file");
    exportAction_->setToolTip(tip);
    if (removeClipAction_ != nullptr)
        removeClipAction_->setEnabled(currentClipId_ != 0);
}

void MainWindow::exportSegment()
{
    const auto plan = exportPlan();
    if (plan.empty() || exportThread_ != nullptr)
        return;

    // Stesso contenitore del primo file: si propone "<nome>_taglio.<estensione>" (o "_unione") nella sua cartella.
    const QFileInfo source(QString::fromStdString(plan.front().path));
    const QString extension = source.suffix();
    const QString suffix = plan.size() > 1 ? QStringLiteral("_unione.") : QStringLiteral("_taglio.");
    const QString suggested = source.dir().filePath(source.completeBaseName() + suffix + extension);
    QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("Esporta"), suggested, QStringLiteral("File %1 (*.%2)").arg(extension.toUpper(), extension));
    if (target.isEmpty())
        return;
    if (QFileInfo(target).suffix().compare(extension, Qt::CaseInsensitive) != 0)
        target += QLatin1Char('.') + extension;  // senza ricodifica il formato resta quello dell'originale
    startExport(QDir::toNativeSeparators(target), true);
}

bool MainWindow::startExport(const QString& target, bool showResult, std::chrono::milliseconds slowDown)
{
    const std::vector<core::ClipRanges> plan = exportPlan();
    if (plan.empty() || exportThread_ != nullptr)
        return false;
    auto outcome = std::make_shared<ExportOutcome>();
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    const std::string outputPath = target.toStdString();

    cancelExport_ = false;
    exportThread_ = QThread::create([=, cancel = &cancelExport_] {
        const auto started = std::chrono::steady_clock::now();
        try {
            outcome->result = core::exportClips(plan, outputPath, [=](double fraction) {
                *progress = fraction;
                // Rallentamento solo per le prove, e solo all'inizio: quanto basta perché compaia la finestra.
                if (slowDown.count() > 0 && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500))
                    std::this_thread::sleep_for(slowDown);
                return !cancel->load();
            });
        } catch (const core::ExportCancelled&) {
            outcome->cancelled = true;
        } catch (const std::exception& e) {
            outcome->error = QString::fromUtf8(e.what());
        }
    });
    exportThread_->setParent(this);

    auto* dialog = new QProgressDialog(QStringLiteral("Esportazione di %1...").arg(QFileInfo(target).fileName()),
                                       QStringLiteral("Annulla"), 0, 1000, this);
    dialog->setWindowTitle(QStringLiteral("Esporta segmento"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(300);  // per i segmenti brevi la finestra non fa in tempo a comparire
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    connect(dialog, &QProgressDialog::canceled, this, [this] { cancelExport_ = true; });
    auto* poll = new QTimer(dialog);
    connect(poll, &QTimer::timeout, dialog, [dialog, progress] { dialog->setValue(static_cast<int>(*progress * 1000)); });
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
    const int clips = outcome.result->clipCount;
    const int count = outcome.result->segmentCount;
    QString what = QStringLiteral("Segmento salvato");
    if (clips > 1)
        what = QStringLiteral("%1 file uniti (%2 tratti) salvati").arg(clips).arg(count);
    else if (count > 1)
        what = QStringLiteral("%1 tratti uniti salvati").arg(count);
    QMessageBox::information(
        this, QStringLiteral("Esportazione completata"),
        QStringLiteral("%1 in:\n%2\n\nDurata: %3\nSenza ricodifica: stessa qualità dell'originale.")
            .arg(what, target, QString::fromStdString(core::formatDuration(outcome.result->durationSeconds))));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == clipList_ && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier) {
            // Con il fuoco sull'elenco dei file, Canc toglie il file dal progetto (non il tratto sotto il cursore).
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            if (const int id = selectedClipId(); id != 0)
                removeClip(id);
            return true;
        }
    }
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        static const int playbackKeys[] = {Qt::Key_Space, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home,
                                           Qt::Key_Comma, Qt::Key_Period, Qt::Key_I, Qt::Key_O,
                                           Qt::Key_A, Qt::Key_X, Qt::Key_Delete};
        const bool playbackKey = std::find(std::begin(playbackKeys), std::end(playbackKeys), key->key())
                              != std::end(playbackKeys);
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        // Bloccando l'evento senza accettarlo, il tasto va alla scorciatoia dei menu Riproduzione e Segmento.
        if (playbackKey && (modifiers == Qt::NoModifier || modifiers == Qt::ControlModifier
                            || modifiers == Qt::ShiftModifier))
            return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    openAction_ = fileMenu->addAction(QStringLiteral("&Apri..."), this, &MainWindow::openFile);
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setStatusTip(QStringLiteral("Apre un file in un nuovo progetto"));
    QAction* addAction = fileMenu->addAction(QStringLiteral("A&ggiungi file..."), this, &MainWindow::addFilesDialog);
    addAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    addAction->setStatusTip(QStringLiteral("Aggiunge altri file al progetto, da unire in fondo"));
    removeClipAction_ = fileMenu->addAction(QStringLiteral("&Togli il file dal progetto"), this, [this] {
        if (currentClipId_ != 0)
            removeClip(currentClipId_);
    });
    fileMenu->addSeparator();

    exportAction_ = fileMenu->addAction(QStringLiteral("&Esporta segmento..."), this, &MainWindow::exportSegment);
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    exportAction_->setStatusTip(QStringLiteral("Salva il segmento fra IN e OUT in un nuovo file, senza ricodifica"));
    updateExportAction();

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QStringLiteral("&Esci"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));

    QMenu* playMenu = menuBar()->addMenu(QStringLiteral("&Riproduzione"));
    QAction* playAction = playMenu->addAction(QStringLiteral("Riproduci / &Pausa"), this,
                                              [this] { player_->togglePlayPause(); });
    playAction->setShortcut(QKeySequence(Qt::Key_Space));
    QAction* backAction = playMenu->addAction(QStringLiteral("&Indietro di 5 secondi"), this,
                                              [this] { player_->seekRelative(-5.0); });
    backAction->setShortcut(QKeySequence(Qt::Key_Left));
    QAction* forwardAction = playMenu->addAction(QStringLiteral("&Avanti di 5 secondi"), this,
                                                 [this] { player_->seekRelative(5.0); });
    forwardAction->setShortcut(QKeySequence(Qt::Key_Right));
    QAction* startAction = playMenu->addAction(QStringLiteral("Vai all'i&nizio"), this, [this] { player_->seekTo(0.0); });
    startAction->setShortcut(QKeySequence(Qt::Key_Home));
    playMenu->addSeparator();
    QAction* previousFrame = playMenu->addAction(QStringLiteral("Fotogramma p&recedente"), this,
                                                 [this] { player_->stepFrame(-1); });
    previousFrame->setShortcuts({QKeySequence(Qt::Key_Comma), QKeySequence(Qt::CTRL | Qt::Key_Left)});
    QAction* nextFrame = playMenu->addAction(QStringLiteral("Fotogramma &successivo"), this,
                                             [this] { player_->stepFrame(+1); });
    nextFrame->setShortcuts({QKeySequence(Qt::Key_Period), QKeySequence(Qt::CTRL | Qt::Key_Right)});

    QMenu* segmentMenu = menuBar()->addMenu(QStringLiteral("&Segmento"));
    QAction* inAction = segmentMenu->addAction(QStringLiteral("Segna &IN (inizio)"), this, [this] { player_->markIn(); });
    inAction->setShortcut(QKeySequence(Qt::Key_I));
    QAction* outAction = segmentMenu->addAction(QStringLiteral("Segna &OUT (fine)"), this, [this] { player_->markOut(); });
    outAction->setShortcut(QKeySequence(Qt::Key_O));
    segmentMenu->addSeparator();
    QAction* goIn = segmentMenu->addAction(QStringLiteral("Vai a IN"), this, [this] { player_->goToIn(); });
    goIn->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_I));
    QAction* goOut = segmentMenu->addAction(QStringLiteral("Vai a OUT"), this, [this] { player_->goToOut(); });
    goOut->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_O));
    segmentMenu->addSeparator();
    segmentMenu->addAction(QStringLiteral("&Cancella IN e OUT"), this, [this] { player_->clearSegment(); });

    // Più tratti dello stesso file, esportati uniti (Ctrl+E).
    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Modifica"));
    QAction* addRange = editMenu->addAction(QStringLiteral("&Aggiungi tratto (IN-OUT)"), this, [this] { player_->addRange(); });
    addRange->setShortcut(QKeySequence(Qt::Key_A));
    QAction* cutRange = editMenu->addAction(QStringLiteral("&Taglia via tratto (IN-OUT)"), this, [this] { player_->cutRange(); });
    cutRange->setShortcut(QKeySequence(Qt::Key_X));
    QAction* deleteRange = editMenu->addAction(QStringLiteral("&Elimina tratto sotto il cursore"), this,
                                               [this] { player_->removeRangeAtPlayhead(); });
    deleteRange->setShortcut(QKeySequence(QKeySequence::Delete));
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("&Svuota elenco tratti"), this, [this] { player_->clearRanges(); });
    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Aiuto"));
    helpMenu->addAction(QStringLiteral("&Informazioni su EditorVideo"), this, &MainWindow::showAbout);
}

namespace {

const QString kVideoFilter = QStringLiteral(
    "File video e audio (*.mp4 *.mkv *.mov *.avi *.m4v *.webm *.ts *.mts *.m2ts *.mpg *.wmv *.flv "
    "*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus);;Tutti i file (*.*)");

QString nativeAbsolute(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

}  // namespace

void MainWindow::openFile()
{
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

void MainWindow::openPath(const QString& path)
{
    // Nuovo progetto: si ferma un'eventuale analisi in corso e si riparte da zero.
    if (probeThread_ != nullptr) {
        cancelProbe_ = true;
        probeThread_->wait();
    }
    probeQueue_.clear();
    project_.clear();
    clipInfo_.clear();
    currentClipId_ = 0;
    currentPath_.clear();
    updatingList_ = true;
    clipList_->clear();
    updatingList_ = false;
    loadingClip_ = true;
    player_->closeFile();
    loadingClip_ = false;
    addFiles({path});
}

void MainWindow::addFiles(const QStringList& paths)
{
    int first = 0;
    for (const QString& path : paths) {
        const int id = project_.add(nativeAbsolute(path).toStdString());
        if (first == 0)
            first = id;
        probeQueue_.push_back(id);
    }
    refreshClipList();
    if (currentClipId_ == 0 && first != 0)
        selectClip(first);
    startNextProbe();
    updateExportAction();
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
                const QString name = QFileInfo(QString::fromStdString(project_.find(id)->path)).fileName();
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
                    loadingClip_ = true;
                    player_->setKeyframes(info.keyframeTimes);
                    loadingClip_ = false;
                }
                refreshClipList();
                updateExportAction();
            }
        }
        startNextProbe();
    });

    QApplication::setOverrideCursor(Qt::BusyCursor);
    statusBar()->showMessage(
        QStringLiteral("Analisi di %1 in corso...").arg(QFileInfo(QString::fromStdString(utf8Path)).fileName()));
    probeThread_->start();
}

void MainWindow::storeCurrentRanges()
{
    if (loadingClip_ || currentClipId_ == 0)
        return;
    core::Clip* clip = project_.find(currentClipId_);
    if (clip != nullptr && clip->analyzed)
        clip->segments = player_->ranges();
}

void MainWindow::selectClip(int id)
{
    const core::Clip* clip = project_.find(id);
    if (clip == nullptr || id == currentClipId_)
        return;
    storeCurrentRanges();

    currentClipId_ = id;
    currentPath_ = QString::fromStdString(clip->path);
    const QString fileName = QFileInfo(currentPath_).fileName();
    setWindowTitle(QStringLiteral("%1 - EditorVideo").arg(fileName));

    // Mentre si carica il file, i cambi nell'anteprima non vanno salvati sul progetto.
    loadingClip_ = true;
    player_->openFile(currentPath_);
    if (clip->analyzed) {
        showInfo(clipInfo_[id]);
        player_->setKeyframes(clipInfo_[id].keyframeTimes);
        player_->setRanges(clip->segments);
    } else {
        infoTree_->clear();
        keyframeList_->clear();
        keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> analisi in corso..."));
    }
    loadingClip_ = false;

    refreshClipList();
    pages_->setCurrentIndex(1);
    updateExportAction();
}

void MainWindow::removeClip(int id)
{
    const int index = project_.indexOf(id);
    if (index < 0)
        return;
    std::erase(probeQueue_, id);
    if (id == probingClipId_)
        cancelProbe_ = true;  // l'analisi finisce da sola e il risultato viene ignorato
    project_.remove(id);
    clipInfo_.erase(id);

    if (id == currentClipId_) {
        currentClipId_ = 0;
        currentPath_.clear();
        loadingClip_ = true;
        player_->closeFile();
        loadingClip_ = false;
        if (!project_.empty()) {
            const int next = project_.clips()[std::min<std::size_t>(index, project_.clips().size() - 1)].id;
            selectClip(next);
        } else {
            infoTree_->clear();
            keyframeList_->clear();
            setWindowTitle(QStringLiteral("EditorVideo"));
            pages_->setCurrentIndex(0);
        }
    }
    refreshClipList();
    updateExportAction();
}

int MainWindow::selectedClipId() const
{
    const QListWidgetItem* item = clipList_->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toInt() : 0;
}

void MainWindow::syncOrderFromList()
{
    // Dopo un trascinamento: il progetto prende l'ordine dell'elenco.
    for (int row = 0; row < clipList_->count(); ++row)
        project_.move(clipList_->item(row)->data(Qt::UserRole).toInt(), row);
    refreshClipList();
    updateExportAction();
}

void MainWindow::refreshClipList()
{
    // L'elenco si ricostruisce dal progetto (numeri, durate e tratti cambiano spesso).
    if (clipList_ == nullptr)
        return;
    updatingList_ = true;
    clipList_->clear();
    int row = 0;
    for (const core::Clip& clip : project_.clips()) {
        const QString name = QFileInfo(QString::fromStdString(clip.path)).fileName();
        QString detail;
        if (!clip.analyzed)
            detail = QStringLiteral("analisi in corso...");
        else if (clip.segments.empty())
            detail = QStringLiteral("%1, tutto").arg(qs(core::formatDuration(clip.outputLength())));
        else
            detail = QStringLiteral("%1, %2 tratti")
                         .arg(qs(core::formatDuration(clip.outputLength())))
                         .arg(clip.segments.ranges().size());
        auto* item = new QListWidgetItem(QStringLiteral("%1. %2  —  %3").arg(++row).arg(name, detail));
        item->setData(Qt::UserRole, clip.id);
        item->setToolTip(QString::fromStdString(clip.path));
        clipList_->addItem(item);
        if (clip.id == currentClipId_)
            clipList_->setCurrentItem(item);
    }
    updatingList_ = false;

    if (project_.empty()) {
        projectLabel_->clear();
        return;
    }
    QString text = QStringLiteral("%1 file  ·  durata finale <b>%2</b>")
                       .arg(project_.clips().size())
                       .arg(qs(core::formatDuration(project_.totalLength())));
    if (!project_.allAnalyzed())
        text += QStringLiteral("  ·  analisi in corso");
    projectLabel_->setText(text);
}
void MainWindow::showInfo(const core::MediaInfo& info)
{
    const QString path = qs(info.path);
    const QString fileName = QFileInfo(path).fileName();
    setWindowTitle(QStringLiteral("%1 - EditorVideo").arg(fileName));

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
    if (info.keyframeStreamIndex >= 0) {
        keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave: %1</b> (flusso %2)")
                                    .arg(info.keyframeTimes.size())
                                    .arg(info.keyframeStreamIndex));
        QList<QTreeWidgetItem*> rows;
        rows.reserve(static_cast<qsizetype>(info.keyframeTimes.size()));
        for (std::size_t i = 0; i < info.keyframeTimes.size(); ++i) {
            auto* row = new QTreeWidgetItem({QString::number(i + 1), qs(core::formatDuration(info.keyframeTimes[i]))});
            row->setData(1, Qt::UserRole, info.keyframeTimes[i]);
            row->setToolTip(1, QStringLiteral("Clic per portare l'anteprima qui"));
            rows.append(row);
        }
        keyframeList_->addTopLevelItems(rows);
    } else {
        keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> nessun flusso video"));
    }

    pages_->setCurrentIndex(1);
    statusBar()->showMessage(QStringLiteral("%1  ·  %2  ·  %3 fotogrammi chiave")
                                 .arg(fileName, qs(core::formatDuration(info.durationSeconds)))
                                 .arg(info.keyframeTimes.size()));
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("Informazioni su EditorVideo"),
                       QStringLiteral("<b>EditorVideo</b><br>Taglio e unione di video senza ricodifica.<br><br>")
                           + versionSummary().toHtmlEscaped());
}

QString versionSummary()
{
    const core::FfmpegInfo ffmpeg = core::ffmpegInfo();
    return QStringLiteral("Qt %1  ·  FFmpeg %2 (libavformat %3, libavcodec %4)")
        .arg(QString::fromLatin1(qVersion()), QString::fromStdString(ffmpeg.version),
             QString::fromStdString(ffmpeg.avformat), QString::fromStdString(ffmpeg.avcodec));
}

}  // namespace editor::app
