#include "EditorApp/MainWindow.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorCore/LibraryInfo.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/SegmentExport.h"

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
#include <QtWidgets/QTabWidget>
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

QString timeText(double seconds)
{
    return qs(core::formatDuration(seconds));
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

const QString kVideoFilter = QStringLiteral(
    "File video e audio (*.mp4 *.mkv *.mov *.avi *.m4v *.webm *.ts *.mts *.m2ts *.mpg *.wmv *.flv "
    "*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus);;Tutti i file (*.*)");

QString nativeAbsolute(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

QString fileNameOf(const std::string& utf8Path)
{
    return QFileInfo(qs(utf8Path)).fileName();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("EditorVideo"));
    resize(1500, 900);

    placeholder_ = new QLabel(QStringLiteral("Apri un file video da File > Apri..."));
    placeholder_->setAlignment(Qt::AlignCenter);

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
    keyframeList_ = new QTreeWidget;
    keyframeList_->setColumnCount(2);
    keyframeList_->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Tempo")});
    keyframeList_->setRootIsDecorated(false);
    keyframeList_->setUniformRowHeights(true);
    keyframeList_->setAlternatingRowColors(true);
    keyframeList_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(keyframeList_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
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
    tabs->addTab(infoTree_, QStringLiteral("Informazioni"));
    tabs->addTab(keyframePanel, QStringLiteral("Fotogrammi chiave"));

    auto* leftPanel = new QSplitter(Qt::Vertical);
    auto* clipPanel = new QWidget;
    auto* clipLayout = new QVBoxLayout(clipPanel);
    clipLayout->setContentsMargins(0, 0, 0, 0);
    clipLayout->addWidget(new QLabel(QStringLiteral("<b>File del progetto</b>")));
    clipLayout->addWidget(clipList_);
    leftPanel->addWidget(clipPanel);
    leftPanel->addWidget(tabs);
    leftPanel->setStretchFactor(0, 1);
    leftPanel->setStretchFactor(1, 2);

    // --- Centro: Sorgente (il file scelto) e Montaggio (il risultato)
    player_ = new PlayerWidget;
    player_->onSegmentChanged = [this] { updateExportAction(); };
    player_->installEventFilter(this);  // un clic sulla Sorgente la rende l'anteprima attiva
    sourceTitle_ = new QLabel(QStringLiteral("<b>Sorgente</b> (segna I e O, poi A per metterlo nel montaggio)"));
    auto* sourcePanel = new QWidget;
    auto* sourceLayout = new QVBoxLayout(sourcePanel);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(sourceTitle_);
    sourceLayout->addWidget(player_, 1);

    program_ = new ProgramMonitor(project_);
    program_->onActivated = [this] { setProgramActive(true); };
    program_->onPositionChanged = [this](double seconds) { timeline_->setPlayhead(seconds); };

    auto* monitors = new QSplitter;
    monitors->addWidget(leftPanel);
    monitors->addWidget(sourcePanel);
    monitors->addWidget(program_);
    monitors->setStretchFactor(0, 1);
    monitors->setStretchFactor(1, 3);
    monitors->setStretchFactor(2, 3);
    monitors->setSizes({320, 590, 590});

    // --- In basso: la sequenza
    timeline_ = new SequenceTimeline;
    timeline_->installEventFilter(this);
    timeline_->onSeek = [this](double seconds) {
        setProgramActive(true);
        program_->seekTo(seconds);
    };
    timeline_->onSelect = [this](int) { setProgramActive(true); };
    timeline_->onMove = [this](int fragmentId, int toIndex) {
        project_.sequence().move(fragmentId, toIndex);
        sequenceChanged();
    };
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
    sequenceLabel_ = new QLabel;
    auto* sequencePanel = new QWidget;
    auto* sequenceLayout = new QVBoxLayout(sequencePanel);
    sequenceLayout->setContentsMargins(0, 0, 0, 0);
    sequenceLayout->addWidget(sequenceLabel_);
    sequenceLayout->addWidget(timeline_);

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(monitors);
    main->addWidget(sequencePanel);
    main->setStretchFactor(0, 4);
    main->setStretchFactor(1, 1);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(placeholder_);
    pages_->addWidget(main);
    setCentralWidget(pages_);

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

// ---------------------------------------------------------------------------------------------------------------
// Anteprima attiva

void MainWindow::setProgramActive(bool active)
{
    programActive_ = active;
    // Il titolo dice a quale anteprima vanno Spazio e frecce.
    sourceTitle_->setText(active ? QStringLiteral("<b>Sorgente</b> (segna I e O, poi A per metterlo nel montaggio)")
                                 : QStringLiteral("<b>▶ Sorgente</b> (segna I e O, poi A per metterlo nel montaggio)"));
}

void MainWindow::togglePlay()
{
    if (programActive_)
        program_->togglePlayPause();
    else
        player_->togglePlayPause();
}

void MainWindow::seekRelative(double seconds)
{
    if (programActive_)
        program_->seekRelative(seconds);
    else
        player_->seekRelative(seconds);
}

void MainWindow::seekStart()
{
    if (programActive_)
        program_->seekTo(0.0);
    else
        player_->seekTo(0.0);
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
    const double in = segment.hasIn() ? segment.in() : clip->snapToKeyframe(0.0);
    const double out = segment.hasOut() ? segment.out() : clip->duration;

    core::Sequence& sequence = project_.sequence();
    const int selected = sequence.indexOf(timeline_->selected());
    const int at = selected >= 0 ? selected + 1 : -1;  // dopo il pezzo selezionato, altrimenti in fondo
    const int id = sequence.insert(at, clip->id, in, out, project_.snapper());
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
    sequenceChanged();
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
    const double from = player_->wantedIn();
    const double to = segment.out();
    QString note;
    switch (project_.sequence().removeSourceRange(clip->id, from, to, project_.snapper())) {
    case core::SegmentList::RemoveResult::Removed:
        note = QStringLiteral("Tolto %1 - %2 dal montaggio. La parte dopo riparte dal fotogramma chiave più vicino.")
                   .arg(timeText(from), timeText(to));
        player_->clearSegment();
        break;
    case core::SegmentList::RemoveResult::OutsideKept:
        note = QStringLiteral("Questo tratto (%1 - %2) non è nel montaggio.").arg(timeText(from), timeText(to));
        break;
    case core::SegmentList::RemoveResult::NoKeyframeInside:
        note = QStringLiteral("Non si può togliere %1 - %2 senza ricodifica: dentro non c'è un fotogramma chiave da "
                              "cui far ripartire il video. Allarga il pezzo fino a comprendere una tacca.")
                   .arg(timeText(from), timeText(to));
        break;
    case core::SegmentList::RemoveResult::Empty:
        note = QStringLiteral("Tratto vuoto: OUT deve stare dopo IN.");
        break;
    }
    player_->showNote(note);
    sequenceChanged();
}

void MainWindow::splitAtPlayhead()
{
    const double at = program_->position();
    const int id = project_.sequence().split(at, project_.snapper());
    if (id == 0) {
        statusBar()->showMessage(QStringLiteral("Qui non si può dividere: nel pezzo non c'è un fotogramma chiave prima "
                                                "del cursore (senza ricodifica un pezzo deve iniziare da lì)."),
                                 8000);
        return;
    }
    const int index = project_.sequence().indexOf(id);
    statusBar()->showMessage(QStringLiteral("Pezzo diviso a %1 (fotogramma chiave più vicino prima del cursore).")
                                 .arg(timeText(project_.sequence().startOf(index))),
                             8000);
    timeline_->setSelected(id);
    sequenceChanged();
}

void MainWindow::deleteSelection()
{
    if (clipList_->hasFocus()) {
        if (const int id = selectedClipId(); id != 0)
            removeClip(id);
        return;
    }
    if (project_.sequence().remove(timeline_->selected())) {
        timeline_->setSelected(0);
        sequenceChanged();
    }
}

void MainWindow::clearSequence()
{
    project_.sequence().clear();
    sequenceChanged();
}

void MainWindow::sequenceChanged()
{
    const core::Sequence& sequence = project_.sequence();
    std::vector<SequenceTimeline::Block> blocks;
    for (const core::Fragment& f : sequence.fragments()) {
        const core::Clip* clip = project_.find(f.clipId);
        blocks.push_back({f.id, project_.indexOf(f.clipId), clip ? fileNameOf(clip->path) : QString(), f.length()});
    }
    timeline_->setBlocks(std::move(blocks));
    sequenceLabel_->setText(sequence.empty()
                                ? QStringLiteral("<b>Montaggio</b>: vuoto")
                                : QStringLiteral("<b>Montaggio</b>: %1 pezzi  ·  durata <b>%2</b>")
                                      .arg(sequence.fragments().size())
                                      .arg(timeText(sequence.totalLength())));
    program_->sequenceChanged();
    updateSourceBands();
    refreshClipList();
    updateExportAction();
}

void MainWindow::updateSourceBands()
{
    // Sulla linea del tempo della Sorgente, in verde i pezzi di questo file che sono nel montaggio.
    const core::Clip* clip = project_.find(currentClipId_);
    if (clip == nullptr || !clip->analyzed)
        return;
    core::SegmentList used;
    used.reset(clip->keyframes, clip->duration);
    for (const core::Fragment& f : project_.sequence().fragments()) {
        if (f.clipId == clip->id)
            used.add(f.start, f.end);
    }
    player_->setRanges(used);
}

// ---------------------------------------------------------------------------------------------------------------
// Esportazione

std::vector<core::ClipRanges> MainWindow::exportPlan() const
{
    if (!project_.sequence().empty())
        return project_.exportPlan();
    // Sequenza vuota: il solo IN-OUT della Sorgente, per un taglio veloce (come allo step 4).
    const core::Clip* clip = project_.find(currentClipId_);
    const core::CutSegment& segment = player_->segment();
    if (clip != nullptr && clip->analyzed && segment.hasIn() && segment.hasOut())
        return {{clip->path, {{segment.in(), segment.out()}}}};
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
    exportAction_->setEnabled(!plan.empty() && exportThread_ == nullptr);
    exportAction_->setText(project_.sequence().empty()
                               ? QStringLiteral("&Esporta segmento IN-OUT...")
                               : QStringLiteral("&Esporta il montaggio (%1 pezzi)...").arg(project_.sequence().fragments().size()));
    exportAction_->setToolTip(plan.empty() ? QStringLiteral("Metti dei pezzi nel montaggio (A) o segna IN e OUT")
                                           : QString());
    if (removeClipAction_ != nullptr)
        removeClipAction_->setEnabled(currentClipId_ != 0);
}

void MainWindow::exportSegment()
{
    const auto plan = exportPlan();
    if (plan.empty() || exportThread_ != nullptr)
        return;

    // Stesso contenitore del primo file: si propone "<nome>_montaggio.<estensione>" (o "_taglio") nella sua cartella.
    const QFileInfo source(qs(plan.front().path));
    const QString extension = source.suffix();
    const QString suffix = project_.sequence().empty() ? QStringLiteral("_taglio.") : QStringLiteral("_montaggio.");
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
    dialog->setWindowTitle(QStringLiteral("Esporta"));
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
    const int count = outcome.result->segmentCount;
    const QString what = count > 1 ? QStringLiteral("Montaggio di %1 pezzi salvato").arg(count)
                                   : QStringLiteral("Segmento salvato");
    QMessageBox::information(
        this, QStringLiteral("Esportazione completata"),
        QStringLiteral("%1 in:\n%2\n\nDurata: %3\nSenza ricodifica: stessa qualità dell'originale.")
            .arg(what, target, timeText(outcome.result->durationSeconds)));
}

// ---------------------------------------------------------------------------------------------------------------
// Tasti

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
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
        static const int commandKeys[] = {Qt::Key_Space, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_Comma,
                                          Qt::Key_Period, Qt::Key_I, Qt::Key_O, Qt::Key_A, Qt::Key_X, Qt::Key_S,
                                          Qt::Key_Delete};
        const bool commandKey = std::find(std::begin(commandKeys), std::end(commandKeys), key->key())
                             != std::end(commandKeys);
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        // Bloccando l'evento senza accettarlo, il tasto va alla scorciatoia del menu (Canc compreso: il menu
        // Montaggio sa se togliere il file o il pezzo).
        if (commandKey && (modifiers == Qt::NoModifier || modifiers == Qt::ControlModifier
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
    addAction->setStatusTip(QStringLiteral("Aggiunge altri file al progetto"));
    removeClipAction_ = fileMenu->addAction(QStringLiteral("&Togli il file dal progetto"), this, [this] {
        if (currentClipId_ != 0)
            removeClip(currentClipId_);
    });
    fileMenu->addSeparator();
    exportAction_ = fileMenu->addAction(QStringLiteral("&Esporta..."), this, &MainWindow::exportSegment);
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    exportAction_->setStatusTip(QStringLiteral("Salva il montaggio in un nuovo file, senza ricodifica"));
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QStringLiteral("&Esci"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));

    QMenu* playMenu = menuBar()->addMenu(QStringLiteral("&Riproduzione"));
    QAction* playAction = playMenu->addAction(QStringLiteral("Riproduci / &Pausa (anteprima attiva)"), this,
                                              [this] { togglePlay(); });
    playAction->setShortcut(QKeySequence(Qt::Key_Space));
    QAction* backAction = playMenu->addAction(QStringLiteral("&Indietro di 5 secondi"), this, [this] { seekRelative(-5.0); });
    backAction->setShortcut(QKeySequence(Qt::Key_Left));
    QAction* forwardAction = playMenu->addAction(QStringLiteral("&Avanti di 5 secondi"), this, [this] { seekRelative(5.0); });
    forwardAction->setShortcut(QKeySequence(Qt::Key_Right));
    QAction* startAction = playMenu->addAction(QStringLiteral("Vai all'i&nizio"), this, [this] { seekStart(); });
    startAction->setShortcut(QKeySequence(Qt::Key_Home));
    playMenu->addSeparator();
    QAction* previousFrame = playMenu->addAction(QStringLiteral("Fotogramma p&recedente (Sorgente)"), this,
                                                 [this] { player_->stepFrame(-1); });
    previousFrame->setShortcuts({QKeySequence(Qt::Key_Comma), QKeySequence(Qt::CTRL | Qt::Key_Left)});
    QAction* nextFrame = playMenu->addAction(QStringLiteral("Fotogramma &successivo (Sorgente)"), this,
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

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Aiuto"));
    helpMenu->addAction(QStringLiteral("&Informazioni su EditorVideo"), this, &MainWindow::showAbout);
    updateExportAction();
}

// ---------------------------------------------------------------------------------------------------------------
// File del progetto

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
    player_->closeFile();
    sequenceChanged();
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
                }
                refreshClipList();
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
        player_->setKeyframes(clip->keyframes);
        updateSourceBands();
    } else {
        infoTree_->clear();
        keyframeList_->clear();
        keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> analisi in corso..."));
        setWindowTitle(QStringLiteral("%1 - EditorVideo").arg(fileNameOf(clip->path)));
    }
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
    project_.remove(id);      // toglie anche i suoi pezzi dal montaggio
    clipInfo_.erase(id);

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
            setWindowTitle(QStringLiteral("EditorVideo"));
            pages_->setCurrentIndex(0);
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
    clipList_->clear();
    for (const core::Clip& clip : project_.clips()) {
        int uses = 0;
        for (const core::Fragment& f : project_.sequence().fragments())
            uses += f.clipId == clip.id ? 1 : 0;
        QString detail = clip.analyzed ? timeText(clip.duration) : QStringLiteral("analisi in corso...");
        if (uses > 0)
            detail += QStringLiteral(", %1 pezzi nel montaggio").arg(uses);
        auto* item = new QListWidgetItem(QStringLiteral("%1  —  %2").arg(fileNameOf(clip.path), detail));
        item->setData(Qt::UserRole, clip.id);
        item->setToolTip(qs(clip.path));
        // Lo stesso colore dei suoi blocchi nel montaggio.
        item->setForeground(blockColor(project_.indexOf(clip.id)));
        clipList_->addItem(item);
        if (clip.id == currentClipId_)
            clipList_->setCurrentItem(item);
    }
    updatingList_ = false;
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
