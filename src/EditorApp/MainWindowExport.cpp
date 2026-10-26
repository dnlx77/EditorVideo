#include "EditorApp/MainWindow.h"

#include "EditorApp/MainWindowShared.h"
#include "EditorApp/QtTextRenderer.h"
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

MainWindow::ExportSettings MainWindow::exportSettings(bool drawText) const
{
    // Audio, formato e ricodifica valgono per il montaggio; il taglio veloce (sequenza vuota) copia il file com'è.
    if (project_.sequence().empty())
        return {};
    ExportSettings settings{project_.audioEdit(core::GapExport::Black), project_.format(), reencodeOptions_, {}};
    // Le scritte: i tratti sotto una scritta si ricodificano col testo sopra.
    if (project_.textTracks().endTime() > 0.0) {
        const QSize frame = frameSize();
        settings.textRenderer =
            std::make_shared<PrerenderedTextRenderer>(project_.textTracks(), frame.width(), frame.height(), drawText);
        settings.reencode.texts = project_.textTracks();
        settings.reencode.textRenderer = settings.textRenderer.get();
    }
    return settings;
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
    const ExportSettings settings = exportSettings(true);
    const core::AudioEdit& audio = settings.audio;
    const core::ProjectFormat& format = settings.format;
    const core::ReencodeOptions& reencode = settings.reencode;
    const std::shared_ptr<const core::TextRenderer> textRenderer = settings.textRenderer;  // vive quanto l'esportazione

    auto outcome = std::make_shared<ExportOutcome>();
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    const std::string outputPath = target.toStdString();

    cancelExport_ = false;
    exportThread_ = QThread::create([=, cancel = &cancelExport_, keepText = textRenderer] {
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
    const int mixedAudioFiles = audio.mix ? core::mixFileCount(*audio.mix) : 0;
    const ExportSettings textSettings = exportSettings();  // per le scritte
    // Quanti tratti si copiano e quanti si ricodificano (la prima volta prova anche gli encoder: qualche secondo).
    const auto preview = [&](bool all, bool closeGaps) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        core::ReencodeOptions options{reencodeOptions_.quality, all, closeGaps};
        options.texts = textSettings.reencode.texts;
        options.textRenderer = textSettings.reencode.textRenderer;
        const core::ExportPreview result = core::previewExport(plan, audio, format.empty() ? nullptr : &format, options);
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
        if (p.textSegments > 0)
            text += QStringLiteral(", <b>%1 con scritte</b> (%2 ricodificati col testo sopra)")
                        .arg(p.textSegments)
                        .arg(timeText(p.textSeconds));
        if (p.blackGaps > 0)
            text += p.blackGaps == 1 ? QStringLiteral(", <b>1 spazio nero</b> (%1)").arg(timeText(p.blackSeconds))
                                     : QStringLiteral(", <b>%1 spazi neri</b> (%2)").arg(p.blackGaps).arg(timeText(p.blackSeconds));
        text += QStringLiteral(".<br>Durata del file: <b>%1</b>.").arg(timeText(p.outputSeconds));
        if (mixedAudioFiles > 0)
            text += QStringLiteral("<br>Audio: <b>mixato e ricodificato</b> in AAC 256 kb/s (%1 file audio), con livelli, linee "
                                   "del livello, muto e solo delle tracce.")
                        .arg(mixedAudioFiles);
        if (!p.closedGaps.empty())
            text += QStringLiteral("<br><span style='color:#d08a1a'>%1</span>").arg(qs(p.closedGaps).toHtmlEscaped());
        if (!p.impreciseCuts.empty())
            text += QStringLiteral("<br><span style='color:#d08a1a'>%1</span>").arg(qs(p.impreciseCuts).toHtmlEscaped());
        if (p.reencodedSegments > 0 && p.copiedSegments > 0)
            text += QStringLiteral("<br>I pezzi copiati non perdono niente; quelli ricodificati sono quasi uguali "
                                   "all'originale con la qualità alta.");
        summary->setText(text);
        quality->setEnabled(p.reencodedSegments > 0 || p.preciseCuts > 0 || p.blackGaps > 0 || p.textSegments > 0);
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
    QString recoding = outcome.result->reencodedSegments == 0
                           ? QStringLiteral("Senza ricodifica del video: stessa qualità dell'originale.")
                           : QStringLiteral("%1 tratti ricodificati, gli altri copiati senza ricodifica.").arg(outcome.result->reencodedSegments);
    if (outcome.result->mixedAudioFiles > 0)
        recoding += QStringLiteral("\nAudio ricodificato: %1 file audio mixati (AAC 256 kb/s).").arg(outcome.result->mixedAudioFiles);
    QMessageBox::information(this, QStringLiteral("Esportazione completata"),
                             QStringLiteral("%1 in:\n%2\n\nDurata: %3\n%4")
                                 .arg(what, target, timeText(outcome.result->durationSeconds), recoding));
}


}  // namespace editor::app
