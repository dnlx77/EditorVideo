#include "EditorApp/ExportDialog.h"

#include "EditorApp/UiText.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace editor::app {

namespace {

constexpr const char* kWarningStyle = "color: #d9b700;";
constexpr const char* kErrorStyle = "color: #e06060;";
constexpr const char* kNoteStyle = "color: #d08a1a;";

QLabel* heading(const QString& text)
{
    auto* label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QFrame* separator()
{
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

QString encoderLabel(const core::VideoEncoderInfo& info)
{
    const bool gpu = info.backend != core::EncoderBackend::Software && info.backend != core::EncoderBackend::MediaFoundation;
    return qs(core::codecName(info.codec)) + QStringLiteral(" – ") + qs(core::backendName(info.backend))
           + (gpu ? QStringLiteral(" (GPU)") : QString());
}

QString bitRateText(int kbps)
{
    return kbps > 0 ? QStringLiteral("%1 kb/s").arg(kbps) : QStringLiteral("senza perdita");
}

}  // namespace

ExportDialog::ExportDialog(const ExportDialogInput& input, QWidget* parent)
    : QDialog(parent)
    , input_(input)
{
    setWindowTitle(QStringLiteral("Esporta"));
    setMinimumWidth(540);
    auto* layout = new QVBoxLayout(this);

    // --- Destinazione
    layout->addWidget(heading(QStringLiteral("Destinazione")));
    auto* destinationRow = new QHBoxLayout;
    destination_ = new QLineEdit(QDir::toNativeSeparators(input_.suggestedPath));
    destinationRow->addWidget(destination_, 1);
    auto* browse = new QPushButton(QStringLiteral("Sfoglia..."));
    destinationRow->addWidget(browse);
    layout->addLayout(destinationRow);
    warning_ = new QLabel;
    warning_->setWordWrap(true);
    layout->addWidget(warning_);
    layout->addWidget(separator());

    // --- Intervallo
    layout->addWidget(heading(QStringLiteral("Intervallo")));
    range_ = new QLabel;
    layout->addWidget(range_);
    layout->addWidget(separator());

    // --- Video
    layout->addWidget(heading(QStringLiteral("Video")));
    auto* videoForm = new QFormLayout;
    encoder_ = new QComboBox;
    preset_ = new QComboBox;
    auto* cqRow = new QHBoxLayout;
    cqSlider_ = new QSlider(Qt::Horizontal);
    cqSpin_ = new QSpinBox;
    cqRow->addWidget(cqSlider_, 1);
    cqRow->addWidget(cqSpin_);
    videoForm->addRow(QStringLiteral("Encoder:"), encoder_);
    videoForm->addRow(QStringLiteral("Preset:"), preset_);
    videoForm->addRow(QStringLiteral("Qualità (CQ, meno = migliore):"), cqRow);
    layout->addLayout(videoForm);
    reencodeAll_ = new QCheckBox(QStringLiteral("Ricodifica tutto (massima compatibilità con i lettori, più lento)"));
    layout->addWidget(reencodeAll_);
    closeGaps_ = new QCheckBox(QStringLiteral("Chiudi gli spazi vuoti (i pezzi uno dopo l'altro, senza nero)"));
    closeGaps_->setVisible(input_.gapCount > 0);
    layout->addWidget(closeGaps_);
    layout->addWidget(separator());

    // --- Audio
    layout->addWidget(heading(QStringLiteral("Audio")));
    includeAudio_ = new QCheckBox(QStringLiteral("Includi audio"));
    includeAudio_->setChecked(input_.options.includeAudio);
    layout->addWidget(includeAudio_);
    auto* audioForm = new QFormLayout;
    audioCodec_ = new QComboBox;
    audioBitRate_ = new QComboBox;
    audioForm->addRow(QStringLiteral("Codec:"), audioCodec_);
    audioForm->addRow(QStringLiteral("Bitrate:"), audioBitRate_);
    layout->addLayout(audioForm);
    audioNote_ = new QLabel;
    audioNote_->setWordWrap(true);
    audioNote_->setStyleSheet(QLatin1String(kNoteStyle));
    layout->addWidget(audioNote_);
    layout->addWidget(separator());

    // --- Riepilogo e pulsanti
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    layout->addWidget(summary_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    exportButton_ = buttons->button(QDialogButtonBox::Ok);
    exportButton_->setText(QStringLiteral("Esporta"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Annulla"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    videoWidgets_ = {encoder_, preset_, cqSlider_, cqSpin_};

    // --- Valori di partenza
    reencodeAll_->setChecked(input_.options.all);
    closeGaps_->setChecked(input_.options.closeGaps);
    cqCustom_ = input_.options.cq > 0;
    updating_ = true;
    fillEncoders();
    updateEncoder();
    if (cqCustom_) {
        cqSpin_->setValue(input_.options.cq);
        cqSlider_->setValue(cqSpin_->value());
    }
    // Codec audio: quelli che il contenitore può avere.
    for (const core::AudioCodec codec : core::availableAudioCodecs()) {
        if (core::audioCodecFitsContainer(codec, input_.extension.toStdString()))
            audioCodec_->addItem(qs(core::audioCodecName(codec)), static_cast<int>(codec));
    }
    if (const int at = audioCodec_->findData(static_cast<int>(input_.options.audioCodec.value_or(core::AudioCodec::Aac))); at >= 0)
        audioCodec_->setCurrentIndex(at);
    fillAudioBitRates();
    updating_ = false;

    // --- Collegamenti
    connect(browse, &QPushButton::clicked, this, [this] { chooseDestination(); });
    connect(destination_, &QLineEdit::textChanged, this, [this] { updateDestination(); });
    connect(encoder_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_)
            updateEncoder();
    });
    connect(cqSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (updating_)
            return;
        cqCustom_ = true;
        updating_ = true;
        cqSpin_->setValue(value);
        updating_ = false;
    });
    connect(cqSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (updating_)
            return;
        cqCustom_ = true;
        updating_ = true;
        cqSlider_->setValue(value);
        updating_ = false;
    });
    connect(reencodeAll_, &QCheckBox::toggled, this, [this] { updatePreview(); });
    connect(closeGaps_, &QCheckBox::toggled, this, [this] { updatePreview(); });
    connect(includeAudio_, &QCheckBox::toggled, this, [this] {
        updatePreview();
        updateAudio();
    });
    connect(audioCodec_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_)
            return;
        updating_ = true;
        fillAudioBitRates();
        updating_ = false;
        updateAudio();
        updateSummary();
    });
    connect(audioBitRate_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_)
            updateSummary();
    });

    updatePreview();
    updateAudio();
    updateDestination();
}

core::AudioCodec ExportDialog::selectedAudioCodec() const
{
    const QVariant data = audioCodec_->currentData();
    return data.isValid() ? static_cast<core::AudioCodec>(data.toInt()) : core::AudioCodec::Aac;
}

std::optional<core::VideoEncoderInfo> ExportDialog::effectiveEncoder() const
{
    if (encoders_.empty())
        return std::nullopt;
    const int index = encoder_->currentData().toInt();
    return index >= 0 && index < static_cast<int>(encoders_.size()) ? encoders_[static_cast<std::size_t>(index)] : encoders_.front();
}

bool ExportDialog::videoChoicesApply() const
{
    return input_.montage && input_.projectCodec.has_value() && !encoders_.empty();
}

void ExportDialog::fillEncoders()
{
    encoders_.clear();
    if (input_.montage && input_.projectCodec) {
        QApplication::setOverrideCursor(Qt::WaitCursor);  // la prima volta prova gli encoder: qualche secondo
        for (const core::VideoEncoderInfo& info : core::availableVideoEncoders()) {
            if (info.codec == *input_.projectCodec)
                encoders_.push_back(info);
        }
        QApplication::restoreOverrideCursor();
    }
    encoder_->clear();
    if (encoders_.empty()) {
        encoder_->addItem(QStringLiteral("Nessun encoder disponibile"), -1);
        return;
    }
    encoder_->addItem(QStringLiteral("Automatico (%1)").arg(encoderLabel(encoders_.front())), -1);
    for (std::size_t i = 0; i < encoders_.size(); ++i)
        encoder_->addItem(encoderLabel(encoders_[i]), static_cast<int>(i));
    if (input_.options.backend) {
        for (std::size_t i = 0; i < encoders_.size(); ++i) {
            if (encoders_[i].backend == *input_.options.backend)
                encoder_->setCurrentIndex(static_cast<int>(i) + 1);
        }
    }
}

void ExportDialog::updateEncoder()
{
    const bool wasUpdating = updating_;
    updating_ = true;
    const std::optional<core::VideoEncoderInfo> info = effectiveEncoder();
    const core::VideoEncoderTuning tuning = info ? core::videoEncoderTuning(*info) : core::VideoEncoderTuning{};
    // Preset: si tiene quello scelto se l'encoder nuovo lo ha (la prima volta quello di partenza).
    const QString keep = preset_->count() > 0 ? preset_->currentData().toString() : qs(input_.options.preset);
    preset_->clear();
    preset_->addItem(QStringLiteral("Predefinito"), QString());
    for (const core::EncoderPreset& preset : tuning.presets)
        preset_->addItem(qs(preset.label), qs(preset.value));
    if (const int at = preset_->findData(keep); at >= 0)
        preset_->setCurrentIndex(at);
    // Qualità: la scala dell'encoder; finché non si tocca il cursore vale il valore della qualità "Alta".
    cqSlider_->setRange(tuning.cqMin, tuning.cqMax);
    cqSpin_->setRange(tuning.cqMin, tuning.cqMax);
    const int value = cqCustom_ ? std::clamp(cqSpin_->value(), tuning.cqMin, tuning.cqMax) : tuning.cqHigh;
    cqSpin_->setValue(value);
    cqSlider_->setValue(value);
    updating_ = wasUpdating;
    if (!updating_)
        updatePreview();  // l'encoder scelto cambia se l'esportazione è possibile
}

void ExportDialog::fillAudioBitRates()
{
    const core::AudioCodec codec = selectedAudioCodec();
    const int keep = audioBitRate_->count() > 0 ? audioBitRate_->currentData().toInt()
                                                : (input_.options.audioBitRate > 0 ? input_.options.audioBitRate / 1000 : 0);
    audioBitRate_->clear();
    const std::vector<int> rates = core::audioBitRatesKbps(codec);
    if (rates.empty()) {
        audioBitRate_->addItem(bitRateText(0), 0);
        return;
    }
    for (const int kbps : rates)
        audioBitRate_->addItem(bitRateText(kbps), kbps);
    // Quello di prima se il codec nuovo lo ha, altrimenti 256 (come il mix di sempre) o il default del codec.
    for (const int wanted : {keep, 256, core::defaultAudioBitRateKbps(codec)}) {
        if (const int at = audioBitRate_->findData(wanted); at >= 0 && wanted > 0) {
            audioBitRate_->setCurrentIndex(at);
            break;
        }
    }
}

void ExportDialog::updateAudio()
{
    const bool mixed = input_.mixedAudioFiles > 0;
    const bool on = includeAudio_->isChecked();
    audioCodec_->setEnabled(on && mixed && audioCodec_->count() > 0);
    audioBitRate_->setEnabled(on && mixed && selectedAudioCodec() != core::AudioCodec::Flac);
    if (!on)
        audioNote_->setText(QStringLiteral("Il file non avrà audio."));
    else if (!mixed)
        audioNote_->setText(QStringLiteral("L'audio dei file si copia senza ricodificarlo: codec e bitrate valgono quando "
                                           "l'audio è mixato (tracce audio aggiunte, livelli)."));
    else
        audioNote_->clear();
    audioNote_->setVisible(!audioNote_->text().isEmpty());
}

core::ReencodeOptions ExportDialog::options() const
{
    core::ReencodeOptions o = input_.options;
    o.all = reencodeAll_->isChecked();
    o.closeGaps = closeGaps_->isChecked();
    if (videoChoicesApply()) {
        const int index = encoder_->currentData().toInt();
        o.backend = index >= 0 && index < static_cast<int>(encoders_.size())
                        ? std::optional<core::EncoderBackend>(encoders_[static_cast<std::size_t>(index)].backend)
                        : std::nullopt;
        o.preset = preset_->currentData().toString().toStdString();
        o.cq = cqCustom_ ? cqSpin_->value() : 0;
    }
    o.includeAudio = includeAudio_->isChecked();
    if (input_.mixedAudioFiles > 0 && audioCodec_->count() > 0) {
        o.audioCodec = selectedAudioCodec();
        o.audioBitRate = audioBitRate_->currentData().toInt() * 1000;
    }
    return o;
}

QString ExportDialog::target() const
{
    QString path = destination_->text().trimmed();
    if (QFileInfo(path).suffix().compare(input_.extension, Qt::CaseInsensitive) != 0)
        path += QLatin1Char('.') + input_.extension;  // il formato resta quello dei file
    return QDir::toNativeSeparators(path);
}

void ExportDialog::setDestination(const QString& path)
{
    destination_->setText(QDir::toNativeSeparators(path));
}

QString ExportDialog::warningText() const
{
    return warning_->text();
}

bool ExportDialog::canExport() const
{
    return exportButton_->isEnabled();
}

void ExportDialog::chooseDestination()
{
    const QString current = destination_->text().trimmed().isEmpty() ? input_.suggestedPath : destination_->text();
    const QString chosen = QFileDialog::getSaveFileName(
        this, QStringLiteral("Scegli la destinazione"), current,
        QStringLiteral("File %1 (*.%2)").arg(input_.extension.toUpper(), input_.extension), nullptr,
        QFileDialog::DontConfirmOverwrite);  // il file che esiste già si dice sotto la destinazione
    if (!chosen.isEmpty())
        setDestination(chosen);
}

void ExportDialog::updateDestination()
{
    const QString text = destination_->text().trimmed();
    QString message;
    bool error = false;
    if (text.isEmpty()) {
        message = QStringLiteral("Scegli dove salvare il file.");
        error = true;
    } else {
        const QFileInfo file(target());
        if (!file.absoluteDir().exists()) {
            message = QStringLiteral("La cartella non esiste.");
            error = true;
        } else if (std::any_of(input_.sources.begin(), input_.sources.end(), [&](const QString& source) {
                       return QFileInfo(source).absoluteFilePath().compare(file.absoluteFilePath(), Qt::CaseInsensitive) == 0;
                   })) {
            message = QStringLiteral("La destinazione è uno dei file originali: scegli un altro nome.");
            error = true;
        } else if (file.exists()) {
            message = QStringLiteral("Il file esiste già e verrà sovrascritto.");
        }
    }
    warning_->setStyleSheet(QLatin1String(error ? kErrorStyle : kWarningStyle));
    warning_->setText(message);
    warning_->setVisible(!message.isEmpty());
    exportButton_->setEnabled(!error && preview_.problem.empty());
}

void ExportDialog::updatePreview()
{
    if (input_.preview) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        preview_ = input_.preview(options());
        QApplication::restoreOverrideCursor();
    }
    updateSummary();
    updateDestination();
}

void ExportDialog::updateSummary()
{
    const core::ExportPreview& p = preview_;
    range_->setText(input_.montage ? QStringLiteral("Tutto il montaggio (%1)").arg(timeText(p.outputSeconds))
                                   : QStringLiteral("Segmento IN-OUT (%1)").arg(timeText(p.outputSeconds)));

    // Le scelte del video valgono se c'è qualcosa da ricodificare.
    const bool recoding = reencodeAll_->isChecked() || p.reencodedSegments > 0 || p.preciseCuts > 0 || p.blackGaps > 0
                          || p.textSegments > 0 || p.transitionSegments > 0;
    const bool apply = videoChoicesApply();
    for (QWidget* widget : videoWidgets_)
        widget->setEnabled(apply && recoding);
    preset_->setEnabled(apply && recoding && preset_->count() > 1);
    reencodeAll_->setEnabled(input_.montage);
    reencodeAll_->setVisible(input_.montage);

    if (!p.problem.empty()) {
        summary_->setText(QStringLiteral("<span style='color:#e06060'>%1</span>").arg(qs(p.problem).toHtmlEscaped()));
        return;
    }
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
    if (p.transitionSegments > 0)
        text += QStringLiteral(", <b>%1 con transizioni</b> (%2 ricodificati con i due pezzi fusi)")
                    .arg(p.transitionSegments)
                    .arg(timeText(p.transitionSeconds));
    if (p.blackGaps > 0)
        text += p.blackGaps == 1 ? QStringLiteral(", <b>1 spazio nero</b> (%1)").arg(timeText(p.blackSeconds))
                                 : QStringLiteral(", <b>%1 spazi neri</b> (%2)").arg(p.blackGaps).arg(timeText(p.blackSeconds));
    text += QStringLiteral(".<br>Durata del file: <b>%1</b>.").arg(timeText(p.outputSeconds));
    if (includeAudio_->isChecked() && input_.mixedAudioFiles > 0) {
        const core::AudioCodec codec = selectedAudioCodec();
        text += QStringLiteral("<br>Audio: <b>mixato e ricodificato</b> in %1 %2 (%3 file audio), con livelli, linee del "
                               "livello, muto e solo delle tracce.")
                    .arg(qs(core::audioCodecName(codec)), bitRateText(codec == core::AudioCodec::Flac ? 0 : audioBitRate_->currentData().toInt()))
                    .arg(input_.mixedAudioFiles);
    }
    if (!p.closedGaps.empty())
        text += QStringLiteral("<br><span style='color:#d08a1a'>%1</span>").arg(qs(p.closedGaps).toHtmlEscaped());
    if (!p.impreciseCuts.empty())
        text += QStringLiteral("<br><span style='color:#d08a1a'>%1</span>").arg(qs(p.impreciseCuts).toHtmlEscaped());
    if (p.reencodedSegments > 0 && p.copiedSegments > 0)
        text += QStringLiteral("<br>I pezzi copiati non perdono niente; quelli ricodificati sono quasi uguali all'originale "
                               "con la qualità alta.");
    summary_->setText(text);
}

}  // namespace editor::app
