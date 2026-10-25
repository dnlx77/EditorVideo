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
            core::relocateMissing(loaded, missing, nativeAbsoluteUtf8(found));
            // Gli altri file mancanti potrebbero essere nella stessa cartella.
            const QDir folder = QFileInfo(found).dir();
            for (const core::MissingFile& other : std::vector<core::MissingFile>(loaded.missing)) {
                const QString candidate = folder.filePath(qs(other.fileName));
                if (QFileInfo::exists(candidate))
                    core::relocateMissing(loaded, other, nativeAbsoluteUtf8(candidate));
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
    for (const auto& [clipId, divisor] : loaded.view.proxyDivisors) {
        if (const core::Clip* clip = project_.find(clipId))
            proxies_->setDivisor(qs(clip->path), divisor);
    }
    for (const core::Clip& clip : project_.clips()) {
        analyzeNewFile(clip.id, clip.path);
    }
    requestAudioWaveforms();
    sequenceChanged();  // aggiorna anche l'elenco dei file
    const int source = project_.find(loaded.view.sourceClipId) != nullptr ? loaded.view.sourceClipId
                     : project_.empty()                                   ? 0
                                                                          : project_.clips().front().id;
    if (source != 0) {
        selectClip(source);
        player_->seekTo(loaded.view.sourcePosition);
    }
    program_->seekTo(loaded.view.programPosition);
    syncQualityMenus();
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

int MainWindow::previewQuality(const QString& path) const
{
    return proxies_->divisor(path);
}

void MainWindow::setPreviewQuality(const QString& path, int divisor)
{
    if (proxies_->divisor(path) == divisor)
        return;
    proxies_->setDivisor(path, divisor);
    setModified(true);  // la qualità di ogni file si salva nel progetto
    syncQualityMenus();
    // Con un proxy già pronto si passa subito; altrimenti si crea in sottofondo (e si passa quando è pronto). Gli altri
    // file non cambiano.
    if (currentPath_ == path)
        player_->reloadVideo();
    program_->reloadVideo();
    updateProxies();
    refreshTimeline();
    const QString name = QFileInfo(path).fileName();
    statusBar()->showMessage(divisor == 1 ? QStringLiteral("Anteprima di %1 dal file originale").arg(name)
                                          : QStringLiteral("Anteprima di %1 dal proxy a 1/%2 della risoluzione (l'esportazione "
                                                           "usa sempre l'originale)")
                                                .arg(name)
                                                .arg(divisor),
                             8000);
}

void MainWindow::syncQualityMenus()
{
    if (!proxies_)
        return;  // in chiusura
    player_->setPreviewQuality(currentPath_.isEmpty() ? 1 : proxies_->divisor(currentPath_));
    const core::Clip* clip = project_.find(program_->currentClipId());
    program_->setPreviewQuality(clip != nullptr ? proxies_->divisor(qs(clip->path)) : 1);
}

void MainWindow::proxyChanged(const QString& original)
{
    if (currentPath_ == original)
        player_->reloadVideo();
    program_->reloadVideo();  // passa al proxy solo se il pezzo mostrato è di quel file
    refreshClipList();
    refreshTimeline();
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
    if (!proxies_->divisors().empty())
        setModified(true);
    proxies_->clearDivisors();
    syncQualityMenus();
    player_->reloadVideo();
    program_->reloadVideo();
    proxies_->removeAll(originals);
    refreshClipList();
    refreshTimeline();
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

}  // namespace editor::app
