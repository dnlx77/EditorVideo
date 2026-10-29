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

// ---------------------------------------------------------------------------------------------------------------
// Audio

void MainWindow::muteRangeForTest(double from, double to)
{
    const core::Project before = project_;
    if (project_.sequence().muteRange(from, to))
        recordEdit(QStringLiteral("Togli audio"), before);
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
    const std::string utf8 = nativeAbsoluteUtf8(path);
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
    const core::AudioTracks& tracks = project_.audioTracks();
    if (showProblems && !project_.audioReplacement() && tracks.endTime() > 0.0
        && QMessageBox::question(this, QStringLiteral("Sostituisci l'audio"),
                                 QStringLiteral("Il file diventa l'audio di tutto il montaggio e le tracce audio aggiunte, "
                                                "con le loro clip, vengono tolte (Ctrl+Z le rimette). Continuare?"))
               != QMessageBox::Yes)
        return false;
    const core::Project before = project_;
    project_.setAudioReplacement(core::AudioReplacement{utf8, 0.0, duration});
    waveforms_->request(utf8);
    statusBar()->showMessage(QStringLiteral("L'audio del montaggio ora è %1, sulla traccia A2 (A1, l'audio originale, è "
                                            "silenziata). Trascina la clip per spostarla.")
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

// ---------------------------------------------------------------------------------------------------------------
// Tracce audio

int MainWindow::colorIndexFor(const std::string& path) const
{
    for (const core::Clip& clip : project_.clips()) {
        if (clip.path == path)
            return project_.indexOf(clip.id);
    }
    return static_cast<int>(qHash(qs(path)) % 10);
}

void MainWindow::analyzeNewFile(int clipId, const std::string& path)
{
    probeQueue_.push_back(clipId);
    waveforms_->request(path);
}

void MainWindow::requestAudioWaveforms()
{
    for (const core::AudioTrack& track : project_.audioTracks().tracks()) {
        for (const core::AudioClip& c : track.clips)
            waveforms_->request(c.path);
    }
}

std::optional<double> MainWindow::audioDurationOf(const std::string& path, QString* problem)
{
    try {
        const core::AudioDecoder decoder(path);
        const double duration = decoder.durationSeconds();
        if (duration > 0.0)
            return duration;
        if (problem != nullptr)
            *problem = QStringLiteral("%1: durata dell'audio sconosciuta.").arg(fileNameOf(path));
    } catch (const std::exception& e) {
        if (problem != nullptr)
            *problem = QStringLiteral("%1: %2").arg(fileNameOf(path), QString::fromUtf8(e.what()));
    }
    return std::nullopt;
}

int MainWindow::addAudioTrack()
{
    const core::Project before = project_;
    const int id = project_.audioTracks().addTrack();
    timeline_->setSelectedTrack(id);
    recordEdit(QStringLiteral("Aggiungi traccia audio"), before);
    statusBar()->showMessage(QStringLiteral("Aggiunta la traccia audio %1: trascinaci un file audio, o usa Audio > Aggiungi "
                                            "file audio al cursore.")
                                 .arg(qs(project_.audioTracks().trackName(id))),
                             8000);
    return id;
}

bool MainWindow::removeAudioTrack(int trackId)
{
    const core::AudioTrack* track = project_.audioTracks().track(trackId);
    if (track == nullptr)
        return false;
    const QString name = qs(project_.audioTracks().trackName(trackId));
    const std::size_t clips = track->clips.size();
    const core::Project before = project_;
    project_.audioTracks().removeTrack(trackId);
    recordEdit(QStringLiteral("Togli traccia audio"), before);
    statusBar()->showMessage(clips == 0 ? QStringLiteral("Tolta la traccia audio %1.").arg(name)
                                        : QStringLiteral("Tolta la traccia audio %1 con %2 clip (Ctrl+Z la rimette).").arg(name).arg(clips),
                             8000);
    return true;
}

int MainWindow::addAudioFiles(const QStringList& paths, int trackId, double seconds)
{
    if (project_.audioTracks().track(trackId) == nullptr)
        return 0;
    std::vector<std::string> utf8;
    for (const QString& path : paths)
        utf8.push_back(nativeAbsoluteUtf8(path));
    const core::DropPlan plan = core::planDrop(utf8, project_, core::inspectMedia);
    QStringList problems;
    for (const std::string& name : plan.unreadable)
        problems.append(QStringLiteral("%1: non è un video o un audio leggibile").arg(qs(name)));

    // Una sola modifica: i file nuovi nell'elenco dei file e le clip sulla traccia, una dopo l'altra.
    const core::Project before = project_;
    std::vector<core::DroppedMedia> withAudio;
    std::vector<double> durations;
    for (const core::DroppedMedia& media : plan.media) {
        QString problem;
        if (const auto duration = audioDurationOf(media.path, &problem)) {
            withAudio.push_back(media);
            durations.push_back(*duration);
        } else {
            problems.append(problem);
        }
    }
    const std::vector<int> ids = core::applyDrop(project_, withAudio, -1);
    core::AudioTracks& audio = project_.audioTracks();
    int added = 0;
    int first = 0;
    double at = std::max(0.0, seconds);
    for (std::size_t i = 0; i < withAudio.size(); ++i) {
        if (withAudio[i].existingClipId == 0)
            analyzeNewFile(ids[i], withAudio[i].path);
        else
            waveforms_->request(withAudio[i].path);
        core::AudioClip clip;
        clip.path = withAudio[i].path;
        clip.end = durations[i];
        clip.sourceDuration = durations[i];
        clip.position = audio.freePosition(trackId, durations[i], at);
        if (const int id = audio.add(trackId, clip); id != 0) {
            ++added;
            first = first == 0 ? id : first;
            at = clip.position + durations[i];
        }
    }
    if (!problems.isEmpty())
        QMessageBox::warning(this, QStringLiteral("File audio non aggiunti"),
                             QStringLiteral("Questi file non sono stati messi sulla traccia:\n• %1").arg(problems.join(QStringLiteral("\n• "))));
    if (added == 0)
        return 0;
    timeline_->setSelectedAudioClip(first);
    recordEdit(QStringLiteral("Aggiungi audio"), before);
    refreshClipList();
    startNextProbe();
    statusBar()->showMessage(QStringLiteral("%1 clip audio sulla traccia %2.").arg(added).arg(qs(audio.trackName(trackId))), 8000);
    return added;
}

void MainWindow::addAudioFilesDialog()
{
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, QStringLiteral("File audio da mettere sulla traccia"), QString(), kAudioFilter);
    if (paths.isEmpty())
        return;
    addAudioFiles(paths, audioTrackForNewClips(), program_->position());
}

int MainWindow::audioTrackForNewClips()
{
    // Sulla traccia scelta, altrimenti sulla prima aggiunta, altrimenti su una nuova.
    int track = timeline_->selectedTrack();
    if (track == core::AudioTracks::kOriginalTrack || project_.audioTracks().track(track) == nullptr)
        track = project_.audioTracks().firstAddedTrack();
    if (track == 0)
        track = addAudioTrack();
    return track;
}

void MainWindow::addSourceAudioToTrack(const core::Clip& clip, double in, double out)
{
    // Un file solo audio non va mai nella traccia video: diventa una clip di una traccia audio, nel tratto IN-OUT, dal
    // cursore del montaggio (nel punto libero più vicino).
    const double length = out - in;
    if (length < kMinAudioClip) {
        player_->showNote(QStringLiteral("Tratto vuoto: OUT deve stare dopo IN."));
        return;
    }
    const int track = audioTrackForNewClips();
    const core::Project before = project_;
    core::AudioTracks& audio = project_.audioTracks();
    core::AudioClip audioClip;
    audioClip.path = clip.path;
    audioClip.start = in;
    audioClip.end = out;
    audioClip.sourceDuration = clip.duration;
    audioClip.position = audio.freePosition(track, length, program_->position());
    const int id = audio.add(track, audioClip);
    if (id == 0) {
        player_->showNote(QStringLiteral("Non c'è posto sulla traccia %1.").arg(qs(audio.trackName(track))));
        return;
    }
    const double position = audio.find(id)->position;
    player_->clearSegment();
    player_->showNote(QStringLiteral("Aggiunto alla traccia %1 del montaggio: %2 - %3, da %4.")
                          .arg(qs(audio.trackName(track)), timeText(in), timeText(out), timeText(position)));
    timeline_->setSelectedAudioClip(id);
    recordEdit(QStringLiteral("Aggiungi audio"), before);
}

bool MainWindow::moveAudioClip(int clipId, int trackId, double position)
{
    core::AudioTracks& audio = project_.audioTracks();
    if (audio.find(clipId) == nullptr)
        return false;
    const core::Project before = project_;
    const std::vector<core::Project::Junction> junctionsBefore = project_.junctions();
    const core::Project::MoveResult moved =
        project_.moveObject({core::ObjectRef::Kind::AudioClip, clipId}, trackId, position, pushPieces_);
    if (!moved.moved)
        return false;
    project_.linkNewJunctions(junctionsBefore);  // clip appena attaccate: compare il pallino comune
    recordEdit(moved.group ? QStringLiteral("Sposta gruppo") : QStringLiteral("Sposta clip audio"), before);
    return true;
}

bool MainWindow::trimAudioClip(int clipId, bool atStart, double value, bool ripple)
{
    const core::Project before = project_;
    if (!project_.audioTracks().trim(clipId, atStart ? core::Sequence::Edge::Start : core::Sequence::Edge::End, value,
                                     kMinAudioClip, ripple))
        return false;
    recordEdit(atStart ? QStringLiteral("Sposta inizio della clip audio") : QStringLiteral("Sposta fine della clip audio"), before);
    return true;
}

void MainWindow::moveFragment(int fragmentId, double start)
{
    const core::Project before = project_;
    const core::Project::MoveResult moved =
        project_.moveObject({core::ObjectRef::Kind::Piece, fragmentId}, 0, start, pushPieces_);
    if (moved.moved)
        recordEdit(moved.group ? QStringLiteral("Sposta gruppo") : QStringLiteral("Sposta pezzo"), before);
}

void MainWindow::setTrackMuted(int trackId, bool muted)
{
    const core::Project before = project_;
    if (project_.audioTracks().setMuted(trackId, muted))
        recordMixEdit(muted ? QStringLiteral("Traccia muta") : QStringLiteral("Traccia non più muta"), before);
}

void MainWindow::setTrackSolo(int trackId, bool solo)
{
    const core::Project before = project_;
    if (project_.audioTracks().setSolo(trackId, solo))
        recordMixEdit(solo ? QStringLiteral("Traccia in solo") : QStringLiteral("Traccia non più in solo"), before);
}

void MainWindow::setTrackLevel(int trackId, double db)
{
    const core::Project before = project_;
    if (project_.audioTracks().setLevel(trackId, db))
        recordMixEdit(QStringLiteral("Livello della traccia"), before);
    else
        refreshTimeline();  // il cursore del livello torna dov'era
}

void MainWindow::setEnvelope(bool fragment, int id, const std::vector<core::LevelPoint>& points)
{
    const core::Project before = project_;
    if (fragment ? project_.sequence().setEnvelope(id, points) : project_.audioTracks().setClipEnvelope(id, points))
        recordMixEdit(QStringLiteral("Linea del livello"), before);
    else
        refreshTimeline();  // il punto trascinato torna dov'era
}

void MainWindow::setJunctionLevel(const core::Project::Junction& junction, std::optional<double> db)
{
    const core::Project before = project_;
    if (db ? project_.setJunctionLevel(junction, *db) : project_.removeJunctionPoint(junction))
        recordMixEdit(db ? QStringLiteral("Pallino della giunzione") : QStringLiteral("Toglie il pallino della giunzione"), before);
    else
        refreshTimeline();  // il pallino torna dov'era
}

void MainWindow::relinkAudio()
{
    const int fragment = timeline_->selected();
    if (project_.sequence().find(fragment) == nullptr) {
        statusBar()->showMessage(QStringLiteral("Scegli prima un pezzo video nella linea del tempo."), 6000);
        return;
    }
    const core::Project before = project_;
    if (!project_.relinkAudio(fragment)) {
        statusBar()->showMessage(QStringLiteral("Nessuna clip audio di questo file è allineata al pezzo."), 8000);
        return;
    }
    recordEdit(QStringLiteral("Ricollega audio"), before);
    statusBar()->showMessage(QStringLiteral("L'audio è tornato quello del pezzo."), 8000);
}

std::vector<core::ObjectRef> MainWindow::selectedRefs() const
{
    return timeline_->allSelectedObjects();
}

void MainWindow::groupSelection()
{
    std::vector<core::ObjectRef> refs = selectedRefs();
    // Un oggetto di un gruppo porta con sé tutto il suo gruppo: scegliendo un gruppo e un altro oggetto (o un altro
    // gruppo) si ottiene un gruppo solo con tutto.
    for (const core::ObjectRef& chosen : std::vector<core::ObjectRef>(refs)) {
        for (const core::ObjectRef& mate : project_.groupMembers(chosen)) {
            if (std::find(refs.begin(), refs.end(), mate) == refs.end())
                refs.push_back(mate);
        }
    }
    const core::Project before = project_;
    int relinked = 0;
    const int group = refs.size() < 2 ? 0 : project_.groupObjects(refs, &relinked);
    if (group == 0 && relinked == 0) {
        statusBar()->showMessage(QStringLiteral("Per fare un gruppo scegli almeno due oggetti diversi con Ctrl+clic."), 8000);
        return;
    }
    recordEdit(group != 0 ? QStringLiteral("Crea gruppo") : QStringLiteral("Ricollega audio"), before);
    if (group == 0) {
        statusBar()->showMessage(QStringLiteral("Video e audio di nuovo uniti in un gruppo."), 8000);
        return;
    }
    showGroupStatus();
}

void MainWindow::ungroupSelection()
{
    const std::vector<core::ObjectRef> refs = selectedRefs();
    if (refs.empty()) {
        statusBar()->showMessage(QStringLiteral("Scegli prima un oggetto del gruppo."), 6000);
        return;
    }
    const core::Project before = project_;
    const core::Project::UngroupResult result = project_.ungroupObjects(refs);
    if (result.dissolved == 0 && result.audioUnlinked == 0) {
        statusBar()->showMessage(QStringLiteral("L'oggetto scelto non è in nessun gruppo."), 6000);
        return;
    }
    recordEdit(result.dissolved > 0 ? QStringLiteral("Sciogli gruppo") : QStringLiteral("Scollega audio"), before);
    statusBar()->showMessage(result.dissolved > 0 ? QStringLiteral("Gruppo sciolto.")
                                                  : QStringLiteral("Video e audio non sono più un gruppo: l'audio è una clip libera su A1."),
                             8000);
}

void MainWindow::showGroupStatus()
{
    const std::vector<core::ObjectRef> refs = selectedRefs();
    if (refs.size() != 1)
        return;
    const std::size_t count = project_.groupMembers(refs.front()).size();
    if (count > 1)
        statusBar()->showMessage(QStringLiteral("Gruppo di %1 oggetti: si muovono insieme.").arg(count), 6000);
}

}  // namespace editor::app
