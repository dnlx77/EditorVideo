#pragma once

#include "EditorCore/TextTracks.h"

#include <QtGui/QColor>
#include <QtWidgets/QWidget>
#include <QtWidgets/QPushButton>

#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QPlainTextEdit;
class QToolButton;

namespace editor::app {

// Un pulsante che mostra un colore (anche trasparente) e lo cambia con la finestra dei colori di Qt.
class ColorButton : public QPushButton
{
public:
    ColorButton(const QColor& color, bool withAlpha, QWidget* parent = nullptr);
    QColor color() const { return color_; }
    void setColor(const QColor& color);

    std::function<void()> onColorChanged;  // (il progetto non usa moc: niente segnali propri)

private:
    void updateLook();
    QColor color_;
    bool withAlpha_ = true;
};

// L'editor delle scritte, in un pannello agganciabile: testo, font, grandezza, colore, contorno, ombra, sfondo, posizione,
// durata e dissolvenze della scritta scelta. Ogni cambiamento arriva subito a `onEdited` (l'anteprima è quella del
// Montaggio, dove la scritta si vede sul video vero).
class TextEditorPanel : public QWidget
{
public:
    explicit TextEditorPanel(QWidget* parent = nullptr);

    // La scritta da modificare (nullptr = nessuna: il pannello si disattiva). `styles`: gli stili con nome disponibili
    // (progetto e impostazioni), che si possono applicare, salvare e togliere. I controlli già uguali non si toccano
    // (così non salta il cursore del testo mentre si scrive).
    void setClip(const core::TextClip* clip, std::vector<core::NamedTextStyle> styles);
    bool hasClip() const { return hasClip_; }
    // La scritta come è ora nei controlli (id e posizione nella sequenza restano quelli di partenza).
    core::TextClip clip() const;
    // Gli stili con nome come sono ora nel pannello (con quelli salvati o tolti qui).
    const std::vector<core::NamedTextStyle>& styles() const { return styles_; }
    // Lo stile attuale salvato con questo nome (se c'è già, lo sostituisce); poi è quello scelto.
    void saveStyleAs(const std::string& name);
    void focusText();  // il cursore nel testo, tutto selezionato

    std::function<void(const core::TextClip&)> onEdited;  // un controllo è cambiato (il progetto non usa moc)
    std::function<void(const std::vector<core::NamedTextStyle>&)> onStylesChanged;  // salvato o tolto uno stile

private:
    void build();
    void load(const core::TextClip& clip);
    void loadStyle(const core::TextStyle& style);  // i controlli dello stile (non testo, durata e dissolvenze)
    core::TextStyle currentStyle() const;
    void fillStyleList(const std::string& select);
    void applySelectedStyle();
    void deleteSelectedStyle();
    void changed();  // un controllo è cambiato: lo dice a onEdited
    void setPosition(double x, double y);

    core::TextClip initial_;
    bool hasClip_ = false;
    bool loading_ = false;
    std::string fontFamily_;  // quello scelto (resta anche se il font non è installato)

    QPlainTextEdit* text_ = nullptr;
    QFontComboBox* font_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QToolButton* bold_ = nullptr;
    QToolButton* italic_ = nullptr;
    QToolButton* underline_ = nullptr;
    QComboBox* align_ = nullptr;
    QDoubleSpinBox* lineSpacing_ = nullptr;
    ColorButton* color_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QCheckBox* outline_ = nullptr;
    ColorButton* outlineColor_ = nullptr;
    QDoubleSpinBox* outlineWidth_ = nullptr;
    QCheckBox* shadow_ = nullptr;
    ColorButton* shadowColor_ = nullptr;
    QDoubleSpinBox* shadowX_ = nullptr;
    QDoubleSpinBox* shadowY_ = nullptr;
    QCheckBox* background_ = nullptr;
    ColorButton* backgroundColor_ = nullptr;
    QDoubleSpinBox* padding_ = nullptr;
    QDoubleSpinBox* centerX_ = nullptr;
    QDoubleSpinBox* centerY_ = nullptr;
    QDoubleSpinBox* length_ = nullptr;
    QDoubleSpinBox* fadeIn_ = nullptr;
    QDoubleSpinBox* fadeOut_ = nullptr;
    std::vector<core::NamedTextStyle> styles_;
    QComboBox* styleList_ = nullptr;
    QLabel* hint_ = nullptr;
    QWidget* controls_ = nullptr;
};

}  // namespace editor::app
