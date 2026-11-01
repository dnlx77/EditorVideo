#pragma once

#include "EditorCore/Encoder.h"
#include "EditorCore/SegmentExport.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;

namespace editor::app {

// Che cosa serve alla finestra "Esporta" per mostrarsi (la riempie la finestra principale).
struct ExportDialogInput
{
    QString suggestedPath;   // destinazione proposta
    QString extension;       // contenitore del file creato, senza punto (resta quello dei file)
    QStringList sources;     // i file originali: non si possono usare come destinazione
    bool montage = false;    // montaggio (sequenza) invece del solo IN-OUT: solo il montaggio si ricodifica
    int gapCount = 0;        // spazi vuoti nel montaggio
    int mixedAudioFiles = 0; // file audio mixati (0 = l'audio dei file si copia)
    std::optional<core::VideoCodec> projectCodec;  // codec del formato del progetto, se si sa ricodificare
    core::ReencodeOptions options;                 // le scelte di partenza
    // Il riepilogo di quello che farebbe l'esportazione con queste scelte (copiati, ricodificati, durata, problemi).
    std::function<core::ExportPreview(const core::ReencodeOptions&)> preview;
};

// La finestra "Esporta": destinazione (con l'avviso se il file c'è già), intervallo, video (encoder, preset, qualità CQ),
// audio (incluso, codec, bitrate) e il riepilogo di quello che si copia e si ricodifica. Si usa con exec(): Accepted =
// esportare in target() con options().
class ExportDialog : public QDialog
{
public:
    ExportDialog(const ExportDialogInput& input, QWidget* parent);

    // Dopo exec(): dove esportare (con l'estensione giusta) e le scelte (partono dalle scelte di input.options).
    QString target() const;
    core::ReencodeOptions options() const;

    // Per le prove automatiche.
    void setDestination(const QString& path);
    QPushButton* exportButton() const { return exportButton_; }
    QString warningText() const;  // l'avviso sotto la destinazione ("Il file esiste già...")
    bool canExport() const;
    QComboBox* encoderCombo() const { return encoder_; }
    QComboBox* audioCodecCombo() const { return audioCodec_; }
    QCheckBox* includeAudioCheck() const { return includeAudio_; }
    QCheckBox* reencodeAllCheck() const { return reencodeAll_; }
    QSlider* cqSlider() const { return cqSlider_; }

private:
    void chooseDestination();
    void updateDestination();  // avviso e pulsante Esporta
    void updatePreview();      // ricalcola il riepilogo (cambiano i tratti da ricodificare)
    void updateSummary();      // riscrive il riepilogo con le scelte attuali
    void fillEncoders();
    void updateEncoder();      // preset e scala del CQ dell'encoder scelto
    void fillAudioBitRates();
    void updateAudio();        // quali scelte audio hanno effetto

    // L'encoder che si userebbe con la scelta attuale (nessuna scelta = il primo funzionante).
    std::optional<core::VideoEncoderInfo> effectiveEncoder() const;
    bool videoChoicesApply() const;  // c'è da ricodificare (o si può scegliere perché si ricodifichi tutto)
    core::AudioCodec selectedAudioCodec() const;

    ExportDialogInput input_;
    core::ExportPreview preview_;
    std::vector<core::VideoEncoderInfo> encoders_;  // quelli del codec del progetto, nell'ordine del menu
    bool cqCustom_ = false;
    bool updating_ = false;

    QLineEdit* destination_ = nullptr;
    QLabel* warning_ = nullptr;
    QLabel* range_ = nullptr;
    QComboBox* encoder_ = nullptr;
    QComboBox* preset_ = nullptr;
    QSlider* cqSlider_ = nullptr;
    QSpinBox* cqSpin_ = nullptr;
    QCheckBox* reencodeAll_ = nullptr;
    QCheckBox* closeGaps_ = nullptr;
    QCheckBox* includeAudio_ = nullptr;
    QComboBox* audioCodec_ = nullptr;
    QComboBox* audioBitRate_ = nullptr;
    QLabel* audioNote_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    std::vector<QWidget*> videoWidgets_;
};

}  // namespace editor::app
