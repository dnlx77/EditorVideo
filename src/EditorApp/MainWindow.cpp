#include "EditorApp/MainWindow.h"

#include "EditorApp/PlayerWidget.h"
#include "EditorCore/LibraryInfo.h"
#include "EditorCore/MediaProbe.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <memory>
#include <optional>

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

    // Anteprima al centro; a destra informazioni sul file (sopra) e fotogrammi chiave (sotto).
    auto* sidePanel = new QSplitter(Qt::Vertical);
    sidePanel->addWidget(infoTree_);
    sidePanel->addWidget(keyframePanel);
    sidePanel->setStretchFactor(0, 2);
    sidePanel->setStretchFactor(1, 1);

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
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool playbackKey = key->key() == Qt::Key_Space || key->key() == Qt::Key_Left
                              || key->key() == Qt::Key_Right || key->key() == Qt::Key_Home;
        // Bloccando l'evento senza accettarlo, il tasto va alla scorciatoia del menu Riproduzione.
        if (playbackKey && key->modifiers() == Qt::NoModifier)
            return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    openAction_ = fileMenu->addAction(QStringLiteral("&Apri..."), this, &MainWindow::openFile);
    openAction_->setShortcut(QKeySequence::Open);

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

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Aiuto"));
    helpMenu->addAction(QStringLiteral("&Informazioni su EditorVideo"), this, &MainWindow::showAbout);
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Apri file video"), QString(),
        QStringLiteral("File video e audio (*.mp4 *.mkv *.mov *.avi *.m4v *.webm *.ts *.mts *.m2ts *.mpg *.wmv *.flv "
                       "*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus);;Tutti i file (*.*)"));
    if (!path.isEmpty())
        openPath(path);
}

void MainWindow::openPath(const QString& path)
{
    if (probeThread_ != nullptr)
        return;  // un file alla volta: il menu è comunque disabilitato durante l'analisi

    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const std::string utf8Path = nativePath.toStdString();  // in Qt 6 toStdString() produce UTF-8
    auto outcome = std::make_shared<ProbeOutcome>();

    cancelProbe_ = false;
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

    connect(probeThread_, &QThread::finished, this, [this, outcome, nativePath] {
        probeThread_->deleteLater();
        probeThread_ = nullptr;
        setBusy(false);

        if (outcome->cancelled)
            return;
        if (!outcome->info) {
            keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> non disponibili"));
            statusBar()->showMessage(QStringLiteral("Impossibile aprire %1").arg(QFileInfo(nativePath).fileName()));
            QMessageBox::warning(this, QStringLiteral("Impossibile aprire il file"), outcome->error);
            return;
        }
        showInfo(*outcome->info);
    });

    setBusy(true);
    statusBar()->showMessage(QStringLiteral("Analisi di %1 in corso...").arg(QFileInfo(nativePath).fileName()));
    probeThread_->start();

    // L'anteprima si apre subito, mentre l'analisi completa (che legge tutto il file) prosegue in background.
    setWindowTitle(QStringLiteral("%1 - EditorVideo").arg(QFileInfo(nativePath).fileName()));
    infoTree_->clear();
    keyframeList_->clear();
    keyframeTitle_->setText(QStringLiteral("<b>Fotogrammi chiave:</b> analisi in corso..."));
    player_->openFile(nativePath);
    pages_->setCurrentIndex(1);
}

void MainWindow::setBusy(bool busy)
{
    openAction_->setEnabled(!busy);
    if (busy)
        QApplication::setOverrideCursor(Qt::BusyCursor);
    else
        QApplication::restoreOverrideCursor();
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
