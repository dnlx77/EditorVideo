#pragma once

#include "EditorCore/AudioEffect.h"

#include <QtWidgets/QDialog>

#include <array>
#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;

namespace editor::app {

class EqGraph;
class EqKnob;

// La finestra di un equalizzatore parametrico a sei bande: grafico della risposta con i punti trascinabili (frequenza e
// guadagno; rotella = Q), per ogni banda tipo, attivazione e manopole Freq/Gain/Q, preset e "Azzera tutto". Il valore si
// consegna a onChanged al rilascio di una manopola o di un punto (un solo passo di Annulla per gesto).
class EqualizerDialog : public QDialog
{
public:
    EqualizerDialog(const QString& title, QWidget* parent);

    void setEffect(const core::AudioEffect& effect);
    core::AudioEffect effect() const;
    std::function<void(const core::AudioEffect&)> onChanged;
    // Lo spettro dell'audio che passa (core::spectrumDb), disegnato dietro la curva; vuoto = non suona.
    void setSpectrum(const std::vector<float>& db);

    // I preset di fabbrica e quelli salvati dall'utente (nome, bande).
    struct Preset
    {
        QString name;
        std::vector<core::EqBand> bands;
        bool builtIn = true;
    };
    static std::vector<Preset> builtInPresets();
    static std::vector<Preset> userPresets();
    static void saveUserPreset(const QString& name, const std::vector<core::EqBand>& bands);
    // Da/verso testo (per i preset salvati).
    static QString bandsToText(const std::vector<core::EqBand>& bands);
    static std::vector<core::EqBand> bandsFromText(const QString& text);

    // Per i test.
    EqGraph* graph() const { return graph_; }
    // Come girare una manopola e rilasciarla (which: 0 freq, 1 gain, 2 Q).
    void turnKnob(int band, int which, double value);
    QComboBox* typeCombo(int band) const { return types_[static_cast<std::size_t>(band)]; }
    QCheckBox* bandCheck(int band) const { return checks_[static_cast<std::size_t>(band)]; }
    QComboBox* presetCombo() const { return presets_; }
    QPushButton* resetAllButton() const { return resetAll_; }

private:
    void commit();
    void bandsChanged(bool final);  // dal grafico o dalle manopole: aggiorna il resto
    void refreshControls();
    void fillPresets();
    void resetBand(int band);

    bool updating_ = false;
    std::vector<core::EqBand> bands_;
    QCheckBox* enabled_ = nullptr;
    QComboBox* presets_ = nullptr;
    QPushButton* resetAll_ = nullptr;
    QPushButton* savePreset_ = nullptr;
    EqGraph* graph_ = nullptr;
    std::array<QCheckBox*, core::kEqBandCount> checks_ = {};
    std::array<QComboBox*, core::kEqBandCount> types_ = {};
    std::array<EqKnob*, core::kEqBandCount * 3> knobs_ = {};
    std::array<QDoubleSpinBox*, core::kEqBandCount * 3> spins_ = {};
    std::vector<Preset> presetList_;
};

}  // namespace editor::app
