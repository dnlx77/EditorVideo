#include "EditorApp/ExportDialog.h"

#include "EditorApp/UiText.h"
#include "EditorCore/ExportEstimate.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace editor::app {

namespace {

constexpr const char* kWarningStyle = "color: #d9b700;";
constexpr const char* kErrorStyle = "color: #e06060;";
constexpr const char* kNoteStyle = "color: #d08a1a;";

// I contenitori che si scelgono nella finestra (l'estensione del file).
const QStringList& exportContainers()
{
    static const QStringList containers{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov")};
    return containers;
}

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

// Un profilo: le scelte della finestra che si possono salvare con un nome. Vuoto / 0 / -1 = "come i file" o "automatico".
struct Profile
{
    QString name;
    bool builtin = false;
    QString container;       // "mp4", "mkv", "mov"; vuoto = come i file
    int codec = -1;          // VideoCodec; -1 = come i file
    QString resolution;      // "p50", "h1080"...; vuoto = come i file
    double frameRate = 0.0;  // 0 = come i file
    int decoder = 0;         // DecoderMode
    QString preset;          // vuoto = predefinito
    int cq = 0;              // 0 = la qualità "alta" dell'encoder
    bool includeAudio = true;
    int audioCodec = -1;     // AudioCodec; -1 = quello attuale
    int audioKbps = 0;       // 0 = quello attuale
};

const char* const kProfilesKey = "esportazione/profili";
QString g_profilesApplication = QStringLiteral("EditorVideo");  // dove si salvano i profili (la prova ne usa un altro)

QSettings profileSettings()
{
    return settings(g_profilesApplication);
}

std::vector<Profile> builtinProfiles()
{
    Profile youtube;
    youtube.name = QStringLiteral("YouTube 1080p");
    youtube.builtin = true;
    youtube.container = QStringLiteral("mp4");
    youtube.codec = static_cast<int>(core::VideoCodec::H264);
    youtube.resolution = QStringLiteral("h1080");
    youtube.audioCodec = static_cast<int>(core::AudioCodec::Aac);
    youtube.audioKbps = 192;

    Profile small;
    small.name = QStringLiteral("Piccolo per WhatsApp");
    small.builtin = true;
    small.container = QStringLiteral("mp4");
    small.codec = static_cast<int>(core::VideoCodec::H264);
    small.resolution = QStringLiteral("h480");
    small.frameRate = 30.0;
    small.cq = 28;
    small.audioCodec = static_cast<int>(core::AudioCodec::Aac);
    small.audioKbps = 96;

    Profile archive;
    archive.name = QStringLiteral("Archivio, qualità massima");
    archive.builtin = true;
    archive.container = QStringLiteral("mkv");
    archive.cq = 16;
    archive.audioCodec = static_cast<int>(core::AudioCodec::Aac);
    archive.audioKbps = 320;
    return {youtube, small, archive};
}

QJsonObject toJson(const Profile& p)
{
    QJsonObject o;
    o[QStringLiteral("nome")] = p.name;
    o[QStringLiteral("contenitore")] = p.container;
    o[QStringLiteral("codec")] = p.codec;
    o[QStringLiteral("risoluzione")] = p.resolution;
    o[QStringLiteral("fps")] = p.frameRate;
    o[QStringLiteral("decoder")] = p.decoder;
    o[QStringLiteral("preset")] = p.preset;
    o[QStringLiteral("cq")] = p.cq;
    o[QStringLiteral("audio")] = p.includeAudio;
    o[QStringLiteral("codecAudio")] = p.audioCodec;
    o[QStringLiteral("kbpsAudio")] = p.audioKbps;
    return o;
}

Profile fromJson(const QJsonObject& o)
{
    Profile p;
    p.name = o.value(QStringLiteral("nome")).toString();
    p.container = o.value(QStringLiteral("contenitore")).toString();
    p.codec = o.value(QStringLiteral("codec")).toInt(-1);
    p.resolution = o.value(QStringLiteral("risoluzione")).toString();
    p.frameRate = o.value(QStringLiteral("fps")).toDouble();
    p.decoder = o.value(QStringLiteral("decoder")).toInt();
    p.preset = o.value(QStringLiteral("preset")).toString();
    p.cq = o.value(QStringLiteral("cq")).toInt();
    p.includeAudio = o.value(QStringLiteral("audio")).toBool(true);
    p.audioCodec = o.value(QStringLiteral("codecAudio")).toInt(-1);
    p.audioKbps = o.value(QStringLiteral("kbpsAudio")).toInt();
    return p;
}

std::vector<Profile> savedProfiles()
{
    std::vector<Profile> result;
    const QJsonDocument doc = QJsonDocument::fromJson(profileSettings().value(kProfilesKey).toString().toUtf8());
    for (const QJsonValue& value : doc.array()) {
        Profile p = fromJson(value.toObject());
        if (!p.name.isEmpty())
            result.push_back(p);
    }
    return result;
}

void storeProfiles(const std::vector<Profile>& profiles)
{
    QJsonArray array;
    for (const Profile& p : profiles)
        array.append(toJson(p));
    profileSettings().setValue(kProfilesKey, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

std::optional<Profile> profileNamed(const QString& name)
{
    if (name.isEmpty())
        return std::nullopt;
    for (const Profile& p : builtinProfiles()) {
        if (p.name == name)
            return p;
    }
    for (const Profile& p : savedProfiles()) {
        if (p.name == name)
            return p;
    }
    return std::nullopt;
}

// Le scelte di un profilo in una riga sola, per sapere se la finestra ne è ancora uguale (il nome non conta).
QString profileKey(const Profile& p)
{
    return QStringList{p.container, QString::number(p.codec), p.resolution, QString::number(p.frameRate), QString::number(p.decoder),
                       p.preset, QString::number(p.cq), QString::number(p.includeAudio), QString::number(p.audioCodec),
                       QString::number(p.audioKbps)}
        .join(QLatin1Char('|'));
}

QString sizeLabel(std::int64_t bytes)
{
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
    return mb >= 10.0 ? QStringLiteral("%1 MB").arg(std::lround(mb)) : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

}  // namespace

void setExportProfilesStore(const QString& application)
{
    g_profilesApplication = application;
}

ExportDialog::ExportDialog(const ExportDialogInput& input, QWidget* parent)
    : QDialog(parent)
    , input_(input)
{
    setWindowTitle(QStringLiteral("Esporta"));
    setMinimumWidth(540);
    auto* layout = new QVBoxLayout(this);

    buildDestination(layout);
    buildVideo(layout);
    buildAudio(layout);
    buildButtons(layout);
    videoWidgets_ = {encoder_, preset_, cqSlider_, cqSpin_, decoder_};
    fillFormatChoices();

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
    fillAudioCodecs();
    updating_ = false;

    connectSignals();

    fillProfiles();
    updatePreview();
    updateAudio();
    updateDestination();
}

void ExportDialog::buildDestination(QVBoxLayout* layout)
{
    // --- Destinazione
    layout->addWidget(heading(QStringLiteral("Destinazione")));
    auto* destinationRow = new QHBoxLayout;
    destination_ = new QLineEdit(QDir::toNativeSeparators(input_.suggestedPath));
    destinationRow->addWidget(destination_, 1);
    browse_ = new QPushButton(QStringLiteral("Sfoglia..."));
    destinationRow->addWidget(browse_);
    layout->addLayout(destinationRow);
    warning_ = new QLabel;
    warning_->setWordWrap(true);
    layout->addWidget(warning_);
    layout->addWidget(separator());

    // --- Profilo: scelte già pronte (o salvate con un nome)
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Profilo:")));
        profile_ = new QComboBox;
        row->addWidget(profile_, 1);
        saveProfile_ = new QPushButton(QStringLiteral("Salva..."));
        deleteProfile_ = new QPushButton(QStringLiteral("Elimina"));
        row->addWidget(saveProfile_);
        row->addWidget(deleteProfile_);
        auto* holder = new QWidget;
        holder->setLayout(row);
        row->setContentsMargins(0, 0, 0, 0);
        holder->setVisible(input_.montage);  // il taglio veloce non ricodifica: niente da scegliere
        layout->addWidget(holder);
    }

    // --- Intervallo
    layout->addWidget(heading(QStringLiteral("Intervallo")));
    range_ = new QLabel;
    layout->addWidget(range_);
    layout->addWidget(separator());
}

void ExportDialog::buildVideo(QVBoxLayout* layout)
{
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
    codec_ = new QComboBox;
    resolution_ = new QComboBox;
    frameRate_ = new QComboBox;
    container_ = new QComboBox;
    videoForm->addRow(QStringLiteral("Contenitore:"), container_);
    videoForm->addRow(QStringLiteral("Codec:"), codec_);
    videoForm->addRow(QStringLiteral("Risoluzione:"), resolution_);
    videoForm->addRow(QStringLiteral("Fotogrammi al secondo:"), frameRate_);
    decoder_ = new QComboBox;
    decoder_->addItem(QStringLiteral("CPU"), static_cast<int>(core::DecoderMode::Software));
    decoder_->addItem(QStringLiteral("GPU (se la scheda lo gestisce, altrimenti CPU)"), static_cast<int>(core::DecoderMode::Hardware));
    decoder_->setCurrentIndex(input_.options.decoder == core::DecoderMode::Hardware ? 1 : 0);
    videoForm->addRow(QStringLiteral("Decoder:"), decoder_);
    compositor_ = new QComboBox;
    compositor_->addItem(QStringLiteral("Automatica (GPU se disponibile, altrimenti CPU)"), static_cast<int>(core::CompositorMode::Auto));
    compositor_->addItem(QStringLiteral("Solo CPU"), static_cast<int>(core::CompositorMode::CpuOnly));
    compositor_->setCurrentIndex(input_.options.compositor == core::CompositorMode::CpuOnly ? 1 : 0);
    compositor_->setToolTip(QStringLiteral("Chi compone trasformazioni, livelli, scritte e transizioni sopra i fotogrammi."));
    videoForm->addRow(QStringLiteral("Composizione:"), compositor_);
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
}

void ExportDialog::buildAudio(QVBoxLayout* layout)
{
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
}

void ExportDialog::buildButtons(QVBoxLayout* layout)
{
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
}

void ExportDialog::fillFormatChoices()
{
    // --- Formato del file: contenitore, codec, risoluzione e fotogrammi al secondo (la prima voce = come i file)
    const bool canChange = input_.montage && input_.projectCodec.has_value();
    container_->addItem(QStringLiteral("Come i file (%1)").arg(input_.extension.toUpper()), QString());
    for (const QString& ext : exportContainers())
        container_->addItem(ext.toUpper(), ext);
    codec_->addItem(QStringLiteral("Come i file (%1)").arg(qs(core::codecName(input_.projectCodec.value_or(core::VideoCodec::H264)))), -1);
    codec_->addItem(QStringLiteral("H.264"), static_cast<int>(core::VideoCodec::H264));
    codec_->addItem(QStringLiteral("HEVC (H.265)"), static_cast<int>(core::VideoCodec::Hevc));
    // AV1 solo se su questo PC c'è un encoder che lo sa fare.
    {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const std::vector<core::VideoEncoderInfo> all = core::availableVideoEncoders();
        QApplication::restoreOverrideCursor();
        if (std::any_of(all.begin(), all.end(), [](const core::VideoEncoderInfo& info) { return info.codec == core::VideoCodec::Av1; }))
            codec_->addItem(QStringLiteral("AV1"), static_cast<int>(core::VideoCodec::Av1));
    }
    resolution_->addItem(input_.sourceWidth > 0 ? QStringLiteral("Come i file (%1x%2)").arg(input_.sourceWidth).arg(input_.sourceHeight)
                                                : QStringLiteral("Come i file"),
                         QString());
    for (const int percent : {75, 50, 25})
        resolution_->addItem(QStringLiteral("%1%").arg(percent), QStringLiteral("p%1").arg(percent));
    for (const int height : {2160, 1440, 1080, 720, 480, 360})
        resolution_->addItem(QStringLiteral("%1p").arg(height), QStringLiteral("h%1").arg(height));
    frameRate_->addItem(input_.sourceFrameRate > 0 ? QStringLiteral("Come i file (%1)").arg(input_.sourceFrameRate, 0, 'f', 2) : QStringLiteral("Come i file"),
                        0.0);
    for (const double rate : {24.0, 25.0, 30.0, 50.0, 60.0})
        frameRate_->addItem(QString::number(rate, 'f', 0), rate);
    container_->setEnabled(input_.montage);
    codec_->setEnabled(canChange);
    resolution_->setEnabled(canChange);
    frameRate_->setEnabled(canChange);
    {
        const core::VideoChange& v = input_.options.video;
        if (v.codec)
            codec_->setCurrentIndex(codec_->findData(static_cast<int>(*v.codec)));
        if (v.frameRate > 0)
            frameRate_->setCurrentIndex(std::max(0, frameRate_->findData(v.frameRate)));
    }
}

void ExportDialog::connectSignals()
{
    // --- Collegamenti
    connect(browse_, &QPushButton::clicked, this, [this] { chooseDestination(); });
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
        updateSummary();
    });
    connect(cqSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (updating_)
            return;
        cqCustom_ = true;
        updating_ = true;
        cqSlider_->setValue(value);
        updating_ = false;
        updateSummary();
    });
    connect(profile_, &QComboBox::currentIndexChanged, this, [this] { applyProfile(); });
    connect(saveProfile_, &QPushButton::clicked, this, [this] { saveProfile(); });
    connect(deleteProfile_, &QPushButton::clicked, this, [this] { deleteProfile(); });
    connect(codec_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_)
            return;
        updating_ = true;
        fillEncoders();
        updating_ = false;
        updateEncoder();
    });
    connect(decoder_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_)
            updatePreview();
    });
    connect(resolution_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_)
            updatePreview();
    });
    connect(frameRate_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_)
            updatePreview();
    });
    connect(container_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_)
            changeContainer();
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
}

void ExportDialog::fillAudioCodecs()
{
    const core::AudioCodec keep = audioCodec_->count() > 0 ? selectedAudioCodec()
                                                           : input_.options.audioCodec.value_or(core::AudioCodec::Aac);
    audioCodec_->clear();
    for (const core::AudioCodec codec : core::availableAudioCodecs()) {
        if (core::audioCodecFitsContainer(codec, extension().toStdString()))
            audioCodec_->addItem(qs(core::audioCodecName(codec)), static_cast<int>(codec));
    }
    if (const int at = audioCodec_->findData(static_cast<int>(keep)); at >= 0)
        audioCodec_->setCurrentIndex(at);
    fillAudioBitRates();
}

std::optional<core::VideoCodec> ExportDialog::effectiveCodec() const
{
    if (codec_ != nullptr && codec_->currentData().toInt() >= 0)
        return static_cast<core::VideoCodec>(codec_->currentData().toInt());
    return input_.projectCodec;
}

QString ExportDialog::extension() const
{
    const QString chosen = container_ != nullptr ? container_->currentData().toString() : QString();
    return chosen.isEmpty() ? input_.extension : chosen;
}

// La misura scelta (pari, proporzioni dei file) e gli altri cambi del video; vuoto = come i file.
core::VideoChange ExportDialog::videoChange() const
{
    core::VideoChange change;
    if (!input_.montage || !input_.projectCodec)
        return change;
    if (codec_->currentData().toInt() >= 0)
        change.codec = static_cast<core::VideoCodec>(codec_->currentData().toInt());
    change.frameRate = frameRate_->currentData().toDouble();
    const QString size = resolution_->currentData().toString();
    if (!size.isEmpty() && input_.sourceWidth > 0 && input_.sourceHeight > 0) {
        const int value = size.mid(1).toInt();
        const double scale = size.startsWith(QLatin1Char('p')) ? value / 100.0 : static_cast<double>(value) / input_.sourceHeight;
        const auto even = [](double v) { return std::max(2, static_cast<int>(std::lround(v / 2.0)) * 2); };
        change.width = even(input_.sourceWidth * scale);
        change.height = even(input_.sourceHeight * scale);
    }
    return change;
}

core::AudioCodec ExportDialog::selectedAudioCodec() const
{
    const QVariant value = audioCodec_->currentData();
    return value.isValid() ? static_cast<core::AudioCodec>(value.toInt()) : core::AudioCodec::Aac;
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
    return input_.montage && effectiveCodec().has_value() && !encoders_.empty();
}

void ExportDialog::fillEncoders()
{
    encoders_.clear();
    const std::optional<core::VideoCodec> wanted = effectiveCodec();
    if (input_.montage && wanted) {
        QApplication::setOverrideCursor(Qt::WaitCursor);  // la prima volta prova gli encoder: qualche secondo
        for (const core::VideoEncoderInfo& info : core::availableVideoEncoders()) {
            if (info.codec == *wanted)
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

void ExportDialog::changeContainer()
{
    // L'estensione del nome segue il contenitore (se il nome ne ha una dei contenitori che si scelgono).
    QString path = destination_->text().trimmed();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (!path.isEmpty() && (exportContainers().contains(suffix) || suffix == input_.extension.toLower())) {
        path.chop(suffix.size());
        destination_->setText(path + extension());
    }
    updating_ = true;
    fillAudioCodecs();
    updating_ = false;
    updateAudio();
    updatePreview();
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
    o.video = videoChange();
    o.decoder = static_cast<core::DecoderMode>(decoder_->currentData().toInt());
    o.compositor = static_cast<core::CompositorMode>(compositor_->currentData().toInt());
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
    if (QFileInfo(path).suffix().compare(extension(), Qt::CaseInsensitive) != 0)
        path += QLatin1Char('.') + extension();
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
        QStringLiteral("File %1 (*.%2)").arg(extension().toUpper(), extension()), nullptr,
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
        } else if (const auto codec = effectiveCodec(); input_.montage && codec && !core::videoCodecFitsContainer(*codec, extension().toStdString())) {
            message = QStringLiteral("Il contenitore %1 non può contenere questo codec: scegline un altro.").arg(extension().toUpper());
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
    followProfile();
    const core::ExportPreview& p = preview_;
    range_->setText(input_.montage ? QStringLiteral("Tutto il montaggio (%1)").arg(timeText(p.outputSeconds))
                                   : QStringLiteral("Segmento IN-OUT (%1)").arg(timeText(p.outputSeconds)));

    // Le scelte del video valgono se c'è qualcosa da ricodificare.
    const bool recoding = reencodeAll_->isChecked() || p.reencodedSegments > 0 || p.preciseCuts > 0 || p.blackGaps > 0
                          || p.textSegments > 0 || p.transitionSegments > 0 || p.transformSegments > 0
                          || p.speedSegments > 0;
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
    if (p.transformSegments > 0)
        text += QStringLiteral(", <b>%1 trasformati</b> (%2 ricodificati con scala, posizione, rotazione o ritaglio)")
                    .arg(p.transformSegments)
                    .arg(timeText(p.transformSeconds));
    if (p.speedSegments > 0)
        text += QStringLiteral(", <b>%1 con velocità</b> (%2 ricodificati alla velocità scelta)")
                    .arg(p.speedSegments)
                    .arg(timeText(p.speedSeconds));
    if (p.transitionSegments > 0)
        text += QStringLiteral(", <b>%1 con transizioni</b> (%2 ricodificati con i due pezzi fusi)")
                    .arg(p.transitionSegments)
                    .arg(timeText(p.transitionSeconds));
    if (p.blackGaps > 0)
        text += p.blackGaps == 1 ? QStringLiteral(", <b>1 spazio nero</b> (%1)").arg(timeText(p.blackSeconds))
                                 : QStringLiteral(", <b>%1 spazi neri</b> (%2)").arg(p.blackGaps).arg(timeText(p.blackSeconds));
    text += QStringLiteral(".<br>Durata del file: <b>%1</b>.").arg(timeText(p.outputSeconds));
    if (const QString size = sizeText(); !size.isEmpty())
        text += QStringLiteral(" Dimensione stimata: <b>%1</b> (stima molto approssimativa: dipende dal contenuto).").arg(size);
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

// Stima della dimensione: i pezzi copiati pesano quanto i file originali (bit/s medi), il resto si calcola dal codec, la
// misura, i fotogrammi al secondo e il CQ scelti, più l'audio.
QString ExportDialog::sizeText() const
{
    const core::ExportPreview& p = preview_;
    if (!p.problem.empty() || p.outputSeconds <= 0.0)
        return {};
    const core::VideoChange change = videoChange();
    core::SizeEstimateInput in;
    in.codec = effectiveCodec().value_or(core::VideoCodec::H264);
    in.width = change.width > 0 ? change.width : input_.sourceWidth;
    in.height = change.height > 0 ? change.height : input_.sourceHeight;
    in.frameRate = change.frameRate > 0.0 ? change.frameRate : input_.sourceFrameRate;
    in.cq = cqSpin_->value();
    in.copiedSeconds = p.copiedSeconds;
    in.recodedSeconds = std::max(0.0, p.outputSeconds - p.copiedSeconds);
    in.copiedBitRate = input_.sourceBitRate;
    if (includeAudio_->isChecked()) {
        const bool chosen = input_.mixedAudioFiles > 0 && audioCodec_->count() > 0;
        in.recodedAudioBitRate = chosen ? (selectedAudioCodec() == core::AudioCodec::Flac ? 800000 : audioBitRate_->currentData().toInt() * 1000)
                                        : 128000;
    }
    const std::int64_t bytes = core::estimateOutputBytes(in);
    return bytes > 0 ? QStringLiteral("circa %1").arg(sizeLabel(bytes)) : QString();
}

void ExportDialog::fillProfiles(const QString& select)
{
    const QSignalBlocker blocker(profile_);
    profile_->clear();
    profile_->addItem(QStringLiteral("Personalizzato"), QString());
    for (const Profile& p : builtinProfiles())
        profile_->addItem(p.name, p.name);
    for (const Profile& p : savedProfiles())
        profile_->addItem(p.name + QStringLiteral(" (mio)"), p.name);
    if (const int at = profile_->findData(select); at >= 0 && !select.isEmpty())
        profile_->setCurrentIndex(at);
    const std::optional<Profile> chosen = profileNamed(profile_->currentData().toString());
    deleteProfile_->setEnabled(chosen && !chosen->builtin);
}

// Le scelte attuali della finestra come profilo (senza nome).
static Profile currentChoices(const ExportDialog& dialog, bool includeAudio, const QString& preset, int cq)
{
    Profile p;
    p.container = dialog.containerCombo()->currentData().toString();
    p.codec = dialog.codecCombo()->currentData().toInt();
    p.resolution = dialog.resolutionCombo()->currentData().toString();
    p.frameRate = dialog.frameRateCombo()->currentData().toDouble();
    p.decoder = dialog.decoderCombo()->currentData().toInt();
    p.preset = preset;
    p.cq = cq;
    p.includeAudio = includeAudio;
    p.audioCodec = dialog.audioCodecCombo()->count() > 0 ? dialog.audioCodecCombo()->currentData().toInt() : -1;
    return p;
}

void ExportDialog::applyProfile()
{
    const std::optional<Profile> chosen = profileNamed(profile_->currentData().toString());
    deleteProfile_->setEnabled(chosen && !chosen->builtin);
    if (!chosen) {
        appliedKey_.clear();
        return;
    }
    const Profile& p = *chosen;
    applyingProfile_ = true;
    updating_ = true;
    const auto select = [](QComboBox* box, const QVariant& value) {
        const int at = box->findData(value);
        if (at >= 0)
            box->setCurrentIndex(at);
        return at >= 0;
    };
    if (container_->isEnabled())
        select(container_, p.container);
    if (codec_->isEnabled())
        select(codec_, p.codec);
    if (resolution_->isEnabled()) {
        // Un'altezza fissa più grande di quella dei file non si usa: ingrandire non migliora niente.
        QString size = p.resolution;
        if (size.startsWith(QLatin1Char('h')) && input_.sourceHeight > 0 && size.mid(1).toInt() >= input_.sourceHeight)
            size.clear();
        select(resolution_, size);
    }
    if (frameRate_->isEnabled())
        select(frameRate_, p.frameRate);
    select(decoder_, p.decoder);

    fillEncoders();
    updateEncoder();
    if (p.cq > 0) {
        cqCustom_ = true;
        cqSpin_->setValue(p.cq);
        cqSlider_->setValue(cqSpin_->value());
    } else {
        cqCustom_ = false;
        updateEncoder();
    }
    select(preset_, p.preset);

    {
        const QSignalBlocker blocker(includeAudio_);
        includeAudio_->setChecked(p.includeAudio);
    }
    fillAudioCodecs();  // quelli che il contenitore scelto può avere
    if (p.audioCodec >= 0 && select(audioCodec_, p.audioCodec))
        fillAudioBitRates();
    if (p.audioKbps > 0)
        select(audioBitRate_, p.audioKbps);
    updating_ = false;

    changeContainer();  // il nome della destinazione segue il contenitore; ricalcola il riepilogo
    updateAudio();
    appliedKey_ = profileKey(currentChoices(*this, includeAudio_->isChecked(), preset_->currentData().toString(), cqCustom_ ? cqSpin_->value() : 0));
    applyingProfile_ = false;
    updateSummary();
}

void ExportDialog::followProfile()
{
    if (applyingProfile_ || profile_ == nullptr || profile_->currentIndex() <= 0 || cqSpin_ == nullptr || preset_ == nullptr)
        return;
    const QString now = profileKey(currentChoices(*this, includeAudio_->isChecked(), preset_->currentData().toString(), cqCustom_ ? cqSpin_->value() : 0));
    if (now == appliedKey_)
        return;
    const QSignalBlocker blocker(profile_);
    profile_->setCurrentIndex(0);
    deleteProfile_->setEnabled(false);
}

void ExportDialog::saveProfile()
{
    const std::optional<Profile> current = profileNamed(profile_->currentData().toString());
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Salva profilo"), QStringLiteral("Nome del profilo:"), QLineEdit::Normal,
                                               current && !current->builtin ? current->name : QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    for (const Profile& builtin : builtinProfiles()) {
        if (builtin.name.compare(name, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, QStringLiteral("Salva profilo"),
                                 QStringLiteral("«%1» è un profilo pronto e non si può sostituire: scegli un altro nome.").arg(name));
            return;
        }
    }
    storeProfileAs(name);
}

void ExportDialog::storeProfileAs(const QString& name)
{
    Profile p = currentChoices(*this, includeAudio_->isChecked(), preset_->currentData().toString(), cqCustom_ ? cqSpin_->value() : 0);
    p.name = name;
    p.audioKbps = audioBitRate_->count() > 0 ? audioBitRate_->currentData().toInt() : 0;
    std::vector<Profile> saved = savedProfiles();
    const auto same = std::find_if(saved.begin(), saved.end(), [&](const Profile& s) { return s.name.compare(name, Qt::CaseInsensitive) == 0; });
    QString stored = name;
    if (same != saved.end()) {
        stored = same->name;  // lo stesso nome (anche con maiuscole diverse): si sostituisce
        p.name = stored;
        *same = p;
    } else {
        saved.push_back(p);  // da qui in poi `same` non vale più
    }
    storeProfiles(saved);
    fillProfiles(stored);
    appliedKey_ = profileKey(currentChoices(*this, includeAudio_->isChecked(), preset_->currentData().toString(), cqCustom_ ? cqSpin_->value() : 0));
}

void ExportDialog::deleteProfile()
{
    const std::optional<Profile> current = profileNamed(profile_->currentData().toString());
    if (!current || current->builtin)
        return;
    if (QMessageBox::question(this, QStringLiteral("Elimina profilo"), QStringLiteral("Eliminare il profilo «%1»?").arg(current->name)) !=
        QMessageBox::Yes)
        return;
    std::vector<Profile> saved = savedProfiles();
    saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const Profile& s) { return s.name == current->name; }), saved.end());
    storeProfiles(saved);
    fillProfiles();
    appliedKey_.clear();
}

}  // namespace editor::app
