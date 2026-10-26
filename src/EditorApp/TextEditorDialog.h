#pragma once

#include "EditorCore/TextTracks.h"

#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtWidgets/QDialog>
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

// L'editor delle scritte: testo, font, grandezza, colore, contorno, ombra, sfondo, posizione e durata, con
// un'anteprima viva (la stessa che si vedrà nel montaggio, disegnata da QtTextRenderer) su un fotogramma di prova della
// stessa forma di quelli del progetto.
class TextEditorDialog : public QDialog
{
public:
    // `clip` è la scritta di partenza; `frameWidth` e `frameHeight` le misure dei fotogrammi del progetto (solo per la
    // forma dell'anteprima e per le proporzioni del testo).
    // `styles`: gli stili con nome disponibili (progetto e impostazioni), che l'utente può applicare, salvare e togliere.
    TextEditorDialog(const core::TextClip& clip, int frameWidth, int frameHeight,
                     std::vector<core::NamedTextStyle> styles = {}, QWidget* parent = nullptr);

    // La scritta come è ora nella finestra (id e posizione nella sequenza restano quelli di partenza).
    core::TextClip clip() const;
    // L'anteprima com'è ora (per le prove).
    QImage previewImage() const { return preview_; }
    // Gli stili con nome come sono ora nella finestra (con quelli salvati o tolti qui).
    const std::vector<core::NamedTextStyle>& styles() const { return styles_; }
    // Lo stile attuale della finestra salvato con questo nome (se c'è già, lo sostituisce); poi è quello scelto.
    void saveStyleAs(const std::string& name);

private:
    void build();
    void load(const core::TextClip& clip);
    void loadStyle(const core::TextStyle& style);  // i controlli dello stile (non testo, durata e dissolvenze)
    core::TextStyle currentStyle() const;
    void fillStyleList(const std::string& select);
    void applySelectedStyle();
    void deleteSelectedStyle();
    void changed();  // un controllo è cambiato: ridisegna l'anteprima
    void renderPreview();
    void setPosition(double x, double y);

    core::TextClip initial_;
    int frameWidth_ = 1920;
    int frameHeight_ = 1080;
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
    QLabel* previewLabel_ = nullptr;
    QImage preview_;
};

}  // namespace editor::app
