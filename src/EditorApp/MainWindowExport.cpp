
#include "EditorApp/ExportDialog.h"
#include "EditorApp/MainWindowShared.h"
#include "EditorApp/QtTextRenderer.h"
#include "EditorCore/StillImage.h"

namespace editor::app {
using namespace detail;

// ---------------------------------------------------------------------------------------------------------------
// Esportazione

const core::Project& MainWindow::exportSource(std::optional<core::Project>& closed) const
{
    // "Chiudi gli spazi vuoti": si toglie solo il vuoto vero; dove c'è una scritta, un livello o dell'audio lo spazio resta.
    if (!reencodeOptions_.closeGaps || project_.sequence().gapCount() == 0)
        return project_;
    closed = project_.withEmptySpaceClosed();
    return *closed;
}

std::vector<core::ClipRanges> MainWindow::exportPlan() const
{
    if (!project_.sequence().empty()) {
        std::optional<core::Project> closed;
        return exportSource(closed).exportPlan(core::GapExport::Black);  // con gli spazi vuoti (neri, o chiusi: vedi exportSource)
    }
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
    std::optional<core::Project> closed;
    const core::Project& source = exportSource(closed);
    ExportSettings settings{source.audioEdit(core::GapExport::Black), source.format(), reencodeOptions_, {}};
    if (closed)
        settings.reencode.closeGaps = false;  // il vuoto vero è già tolto dal progetto; quello con sopra qualcosa resta nero
    // Le scritte: i tratti sotto una scritta si ricodificano col testo sopra.
    if (source.textTracks().endTime() > 0.0) {
        // Alla misura del video esportato: se la risoluzione è ridotta le scritte si disegnano a quella misura.
        QSize frame = frameSize();
        const core::VideoChange& chosen = reencodeOptions_.video;
        if (chosen.width > 0 && chosen.height > 0)
            frame = QSize(chosen.width, chosen.height);
        settings.textRenderer =
            std::make_shared<PrerenderedTextRenderer>(source.textTracks(), frame.width(), frame.height(), drawText);
        settings.reencode.texts = source.textTracks();
        settings.reencode.textRenderer = settings.textRenderer.get();
        // I pezzi di video dei livelli sopra V1: il file originale di ognuno (mai il proxy).
        for (const core::TextLayer& layer : source.textTracks().layers()) {
            for (const core::LayerPiece& piece : layer.pieces) {
                if (const core::Clip* file = source.find(piece.clipId))
                    settings.reencode.layerFiles[piece.clipId] = file->path;
            }
        }
    }
    // Oltre l'ultimo pezzo di V1: scritte, pezzi dei livelli e clip audio che arrivano più in là (nero sotto).
    settings.reencode.tailSeconds =
        std::max(0.0, std::max(source.textTracks().endTime(), source.audioTracks().endTime()) - source.sequence().totalLength());
    // Le transizioni: la parte dei pezzi dentro una transizione si ricodifica con i due pezzi fusi.
    settings.reencode.transitions = core::exportTransitionsOf(source);
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
    const bool montage = !project_.sequence().empty();
    const QFileInfo source(qs(plan.front().path));
    ExportDialogInput input;
    // Un montaggio che inizia con un'immagine non esce come immagine: MP4.
    input.extension = core::isImagePath(plan.front().path) ? QStringLiteral("mp4") : source.suffix();
    input.suggestedPath = QDir(workFolder()).filePath(source.completeBaseName() + (montage ? QStringLiteral("_montaggio.") : QStringLiteral("_taglio."))
                                                      + input.extension);
    for (const core::ClipRanges& clip : plan)
        input.sources << qs(clip.path);
    // Bit al secondo medi dei file usati (dimensione / durata): serve alla stima della dimensione dei pezzi copiati.
    {
        double bytes = 0.0;
        double seconds = 0.0;
        for (const core::Clip& clip : project_.clips()) {
            if (clip.duration > 0.0 && !clip.still && input.sources.contains(qs(clip.path))) {
                bytes += static_cast<double>(QFileInfo(qs(clip.path)).size());
                seconds += clip.duration;
            }
        }
        input.sourceBitRate = seconds > 0.0 ? bytes * 8.0 / seconds : 0.0;
    }
    input.montage = montage;
    input.gapCount = montage ? static_cast<int>(project_.sequence().gapCount()) : 0;
    if (montage) {
        const core::AudioEdit audio = project_.audioEdit(core::GapExport::Black);
        input.mixedAudioFiles = audio.mix ? core::mixFileCount(*audio.mix) : 0;
        if (const core::StreamSignature* video = project_.format().video(); video != nullptr) {
            input.sourceWidth = video->width;
            input.sourceHeight = video->height;
            input.sourceFrameRate = video->frameRate;
            input.projectCodec = core::videoCodecFromName(video->codec);
        }
    }
    loadExportChoices();
    input.options = reencodeOptions_;
    // Il riepilogo: quanti tratti si copiano e quanti si ricodificano con le scelte fatte nella finestra.
    // audio, formato e scritte; la finestra mette le sue scelte. Con e senza "Chiudi gli spazi vuoti" il progetto è diverso
    // (senza il vuoto vero), quindi si preparano tutti e due.
    struct ExportVariant
    {
        ExportSettings base;
        std::vector<core::ClipRanges> plan;
    };
    const bool savedClose = reencodeOptions_.closeGaps;
    reencodeOptions_.closeGaps = false;
    const auto openVariant = std::make_shared<ExportVariant>(ExportVariant{exportSettings(), previewPlan()});
    auto closedVariant = openVariant;
    if (project_.sequence().gapCount() > 0) {
        reencodeOptions_.closeGaps = true;
        closedVariant = std::make_shared<ExportVariant>(ExportVariant{exportSettings(), previewPlan()});
    }
    reencodeOptions_.closeGaps = savedClose;
    input.preview = [openVariant, closedVariant](const core::ReencodeOptions& choices) {
        const ExportVariant& v = choices.closeGaps ? *closedVariant : *openVariant;
        core::ReencodeOptions options = choices;
        options.closeGaps = false;  // già nel progetto della variante
        options.texts = v.base.reencode.texts;
        options.textRenderer = v.base.reencode.textRenderer;
        options.layerFiles = v.base.reencode.layerFiles;
        options.transitions = v.base.reencode.transitions;
        options.tailSeconds = v.base.reencode.tailSeconds;
        return core::previewExport(v.plan, v.base.audio, v.base.format.empty() ? nullptr : &v.base.format, options);
    };

    ExportDialog dialog(input, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    reencodeOptions_ = dialog.options();
    saveExportChoices();
    startExport(dialog.target(), true);
}

// Le scelte dell'encoder e dell'audio si ricordano fra una volta e l'altra (le altre, come "Ricodifica tutto" o togliere
// l'audio, valgono solo per l'esportazione in corso).
void MainWindow::loadExportChoices()
{
    QSettings s = settings();
    const QString encoder = s.value(QStringLiteral("esportazione/encoder")).toString();
    reencodeOptions_.backend.reset();
    for (const core::EncoderBackend backend : {core::EncoderBackend::Nvenc, core::EncoderBackend::Qsv, core::EncoderBackend::Amf,
                                               core::EncoderBackend::MediaFoundation, core::EncoderBackend::Software}) {
        if (encoder == qs(core::backendName(backend)))
            reencodeOptions_.backend = backend;
    }
    reencodeOptions_.preset = s.value(QStringLiteral("esportazione/preset")).toString().toStdString();
    reencodeOptions_.cq = std::max(0, s.value(QStringLiteral("esportazione/cq"), 0).toInt());
    reencodeOptions_.includeAudio = true;
    reencodeOptions_.video = {};  // codec, misura e fotogrammi al secondo valgono solo per l'esportazione in corso
    reencodeOptions_.decoder = s.value(QStringLiteral("esportazione/decoder")).toString() == QLatin1String("gpu")
                                   ? core::DecoderMode::Hardware
                                   : core::DecoderMode::Software;
    reencodeOptions_.audioCodec.reset();
    const QString codec = s.value(QStringLiteral("esportazione/codecAudio")).toString();
    for (const core::AudioCodec candidate : core::availableAudioCodecs()) {
        if (codec == qs(core::audioCodecName(candidate)))
            reencodeOptions_.audioCodec = candidate;
    }
    reencodeOptions_.audioBitRate = std::max(0, s.value(QStringLiteral("esportazione/bitrateAudio"), 0).toInt());
}

void MainWindow::saveExportChoices()
{
    QSettings s = settings();
    s.setValue(QStringLiteral("esportazione/encoder"),
               reencodeOptions_.backend ? qs(core::backendName(*reencodeOptions_.backend)) : QString());
    s.setValue(QStringLiteral("esportazione/preset"), qs(reencodeOptions_.preset));
    s.setValue(QStringLiteral("esportazione/cq"), reencodeOptions_.cq);
    s.setValue(QStringLiteral("esportazione/decoder"),
               reencodeOptions_.decoder == core::DecoderMode::Hardware ? QStringLiteral("gpu") : QStringLiteral("cpu"));
    if (reencodeOptions_.audioCodec) {
        s.setValue(QStringLiteral("esportazione/codecAudio"), qs(core::audioCodecName(*reencodeOptions_.audioCodec)));
        s.setValue(QStringLiteral("esportazione/bitrateAudio"), reencodeOptions_.audioBitRate);
    }
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

namespace {

// "AAC 256 kb/s", "FLAC" ...: l'audio mixato come si è scelto.
QString audioChoiceText(const core::ReencodeOptions& options)
{
    const core::AudioCodec codec = options.audioCodec.value_or(core::AudioCodec::Aac);
    QString text = qs(core::audioCodecName(codec));
    if (codec != core::AudioCodec::Flac)
        text += QStringLiteral(" %1 kb/s").arg(options.audioBitRate > 0 ? options.audioBitRate / 1000 : 256);
    return text;
}

}  // namespace

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
        recoding += QStringLiteral("\nAudio ricodificato: %1 file audio mixati (%2).")
                        .arg(outcome.result->mixedAudioFiles)
                        .arg(audioChoiceText(reencodeOptions_));
    // Dove è andato il tempo, per capire cosa rallenta l'esportazione.
    const core::ExportResult& r = *outcome.result;
    if (r.totalSeconds > 0.0 && (r.composeSeconds > 0.0 || r.encodeSeconds > 0.0 || r.mixSeconds > 0.0))
        recoding += QStringLiteral("\nTempi: totale %1 s (mix audio %2 s, composizione %3 s, codifica %4 s, lettura e decodifica %5 s).")
                        .arg(r.totalSeconds, 0, 'f', 1)
                        .arg(r.mixSeconds, 0, 'f', 1)
                        .arg(r.composeSeconds, 0, 'f', 1)
                        .arg(r.encodeSeconds, 0, 'f', 1)
                        .arg(std::max(0.0, r.totalSeconds - r.mixSeconds - r.composeSeconds - r.encodeSeconds), 0, 'f', 1);
    if (r.composeSeconds > 0.0)
        recoding += QStringLiteral("\nComposizione: scritte %1 s, livelli %2 s, fusione %3 s.")
                        .arg(r.textPrepSeconds, 0, 'f', 1)
                        .arg(r.layerPrepSeconds, 0, 'f', 1)
                        .arg(r.blendSeconds, 0, 'f', 1);
    if (r.blendSeconds > 0.0)
        recoding += QStringLiteral("\nFusione: in RGBA %1 s, nero %2 s, pezzo trasformato %3 s, livelli e scritte %4 s, in YUV %5 s.")
                        .arg(r.blendToRgbaSeconds, 0, 'f', 1)
                        .arg(r.blendFillSeconds, 0, 'f', 1)
                        .arg(r.blendCompositeSeconds, 0, 'f', 1)
                        .arg(r.blendOverlaySeconds, 0, 'f', 1)
                        .arg(r.blendToYuvSeconds, 0, 'f', 1);
    QMessageBox::information(this, QStringLiteral("Esportazione completata"),
                             QStringLiteral("%1 in:\n%2\n\nDurata: %3\n%4")
                                 .arg(what, target, timeText(outcome.result->durationSeconds), recoding));
}

}  // namespace editor::app
