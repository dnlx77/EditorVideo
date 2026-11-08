#include "EditorApp/TextEditorPanel.h"
#include "EditorApp/QtTextRenderer.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFontComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <QtGui/QLinearGradient>
#include <QtGui/QPixmap>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

core::TextColor fromQt(const QColor& c)
{
    return {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()), static_cast<std::uint8_t>(c.blue()),
            static_cast<std::uint8_t>(c.alpha())};
}

QDoubleSpinBox* spin(double min, double max, double step, int decimals, const QString& suffix, const char* name)
{
    auto* box = new QDoubleSpinBox;
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    box->setObjectName(QString::fromLatin1(name));
    // Stile minimale: caselle strette e basse, senza frecce.
    box->setFixedWidth(64);
    box->setFixedHeight(20);
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return box;
}

QToolButton* styleButton(const QString& text, const QString& tip, const char* name)
{
    auto* button = new QToolButton;
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setObjectName(QString::fromLatin1(name));
    button->setFixedSize(22, 20);
    return button;
}

}  // namespace

ColorButton::ColorButton(const QColor& color, bool withAlpha, QWidget* parent)
    : QPushButton(parent)
    , color_(color)
    , withAlpha_(withAlpha)
{
    setFixedSize(48, 20);
    updateLook();
    connect(this, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(color_, this, QStringLiteral("Colore"),
                                                     withAlpha_ ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
        if (chosen.isValid())
            setColor(chosen);
    });
}

void ColorButton::setColor(const QColor& color)
{
    if (color == color_)
        return;
    color_ = color;
    updateLook();
    if (onColorChanged)
        onColorChanged();
}

void ColorButton::updateLook()
{
    // Sfondo a scacchi sotto al colore, per far vedere la trasparenza.
    QPixmap pixmap(36, 12);
    QPainter p(&pixmap);
    for (int y = 0; y < 12; y += 4) {
        for (int x = 0; x < 36; x += 4)
            p.fillRect(x, y, 4, 4, ((x + y) / 4) % 2 == 0 ? QColor(200, 200, 200) : QColor(255, 255, 255));
    }
    p.fillRect(pixmap.rect(), color_);
    p.end();
    setIcon(QIcon(pixmap));
    setIconSize(pixmap.size());
    setToolTip(QStringLiteral("Colore: %1 (alfa %2)").arg(color_.name(), QString::number(color_.alpha())));
}

TextEditorPanel::TextEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    build();
    fillStyleList(std::string());
    setClip(nullptr, {});
}

void TextEditorPanel::setExtraSection(QWidget* widget)
{
    if (widget == nullptr || widget == extra_ || controlsLayout_ == nullptr)
        return;
    takeExtraSection();
    extra_ = widget;
    controlsLayout_->insertWidget(controlsLayout_->count() - 1, widget);  // prima dello spazio elastico in fondo
    widget->show();
}

QWidget* TextEditorPanel::takeExtraSection()
{
    QWidget* widget = extra_;
    extra_ = nullptr;
    if (widget != nullptr && controlsLayout_ != nullptr) {
        controlsLayout_->removeWidget(widget);
        widget->setParent(nullptr);
    }
    return widget;
}

void TextEditorPanel::build()
{
    // Il testo e, sotto, tutti i controlli dell'aspetto.
    text_ = new QPlainTextEdit;
    text_->setObjectName(QStringLiteral("testo"));
    text_->setPlaceholderText(QStringLiteral("Scrivi qui il testo (Invio va a capo)"));
    text_->setMinimumHeight(48);
    text_->setMaximumHeight(72);

    font_ = new QFontComboBox;
    font_->setObjectName(QStringLiteral("font"));
    size_ = spin(0.5, 100.0, 0.5, 1, QStringLiteral(" %"), "grandezza");
    size_->setToolTip(QStringLiteral("L'altezza del carattere, in percentuale dell'altezza del fotogramma"));
    bold_ = styleButton(QStringLiteral("G"), QStringLiteral("Grassetto"), "grassetto");
    QFont boldFont = bold_->font();
    boldFont.setBold(true);
    bold_->setFont(boldFont);
    italic_ = styleButton(QStringLiteral("C"), QStringLiteral("Corsivo"), "corsivo");
    QFont italicFont = italic_->font();
    italicFont.setItalic(true);
    italic_->setFont(italicFont);
    underline_ = styleButton(QStringLiteral("S"), QStringLiteral("Sottolineato"), "sottolineato");
    QFont underlineFont = underline_->font();
    underlineFont.setUnderline(true);
    underline_->setFont(underlineFont);
    align_ = new QComboBox;
    align_->setObjectName(QStringLiteral("allineamento"));
    align_->addItems({QStringLiteral("A sinistra"), QStringLiteral("Al centro"), QStringLiteral("A destra")});
    lineSpacing_ = spin(0.5, 3.0, 0.05, 2, QString(), "interlinea");
    color_ = new ColorButton(Qt::white, true);
    color_->setObjectName(QStringLiteral("coloreTesto"));
    opacity_ = spin(0.0, 100.0, 5.0, 0, QStringLiteral(" %"), "opacita");

    auto* look = new QWidget;
    auto* lookForm = new QFormLayout(look);
    lookForm->setContentsMargins(0, 0, 0, 0);
    lookForm->setVerticalSpacing(2);
    lookForm->setHorizontalSpacing(6);
    // Stili con nome: si applicano al testo e agli effetti qui sotto, e si possono salvare con un nome.
    styleList_ = new QComboBox;
    styleList_->setObjectName(QStringLiteral("stili"));
    auto* applyStyle = new QPushButton(QStringLiteral("✓"));
    applyStyle->setObjectName(QStringLiteral("applicaStile"));
    applyStyle->setToolTip(QStringLiteral("Usa lo stile scelto (font, colori, effetti, posizione) su questa scritta"));
    auto* saveStyle = new QPushButton(QStringLiteral("+"));
    saveStyle->setObjectName(QStringLiteral("salvaStile"));
    saveStyle->setToolTip(QStringLiteral("Salva l'aspetto di questa scritta come stile con un nome, da riusare"));
    auto* deleteStyle = new QPushButton(QStringLiteral("✕"));
    deleteStyle->setObjectName(QStringLiteral("eliminaStile"));
    deleteStyle->setToolTip(QStringLiteral("Elimina lo stile scelto"));
    for (QPushButton* button : {applyStyle, saveStyle, deleteStyle})
        button->setFixedSize(24, 20);
    auto* styleRow = new QHBoxLayout;
    styleRow->setSpacing(3);
    styleRow->addWidget(styleList_, 1);
    styleRow->addWidget(applyStyle);
    styleRow->addWidget(saveStyle);
    styleRow->addWidget(deleteStyle);
    lookForm->addRow(QStringLiteral("Stile"), styleRow);
    connect(applyStyle, &QPushButton::clicked, this, [this] { applySelectedStyle(); });
    connect(deleteStyle, &QPushButton::clicked, this, [this] { deleteSelectedStyle(); });
    connect(saveStyle, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const QString current = styleList_->currentIndex() > 0 ? styleList_->currentText() : QString();
        const QString name = QInputDialog::getText(this, QStringLiteral("Salva stile"), QStringLiteral("Nome dello stile:"),
                                                   QLineEdit::Normal, current, &accepted)
                                 .trimmed();
        if (accepted && !name.isEmpty())
            saveStyleAs(name.toStdString());
    });
    lookForm->addRow(QStringLiteral("Font"), font_);
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(size_);
    sizeRow->addWidget(bold_);
    sizeRow->addWidget(italic_);
    sizeRow->addWidget(underline_);
    lookForm->addRow(QStringLiteral("Grandezza"), sizeRow);
    lookForm->addRow(QStringLiteral("Allineamento"), align_);
    lookForm->addRow(QStringLiteral("Interlinea"), lineSpacing_);
    auto* colorRow = new QHBoxLayout;
    colorRow->addWidget(color_);
    colorRow->addWidget(new QLabel(QStringLiteral("Opacità")));
    colorRow->addWidget(opacity_);
    lookForm->addRow(QStringLiteral("Colore"), colorRow);

    outline_ = new QCheckBox(QStringLiteral("Contorno"));
    outline_->setObjectName(QStringLiteral("contorno"));
    outlineColor_ = new ColorButton(Qt::black, true);
    outlineColor_->setObjectName(QStringLiteral("coloreContorno"));
    outlineWidth_ = spin(0.0, 10.0, 0.1, 1, QStringLiteral(" %"), "spessoreContorno");
    shadow_ = new QCheckBox(QStringLiteral("Ombra"));
    shadow_->setObjectName(QStringLiteral("ombra"));
    shadowColor_ = new ColorButton(QColor(0, 0, 0, 160), true);
    shadowColor_->setObjectName(QStringLiteral("coloreOmbra"));
    shadowX_ = spin(-10.0, 10.0, 0.1, 1, QStringLiteral(" %"), "ombraX");
    shadowY_ = spin(-10.0, 10.0, 0.1, 1, QStringLiteral(" %"), "ombraY");
    shadowX_->setFixedWidth(52);
    shadowY_->setFixedWidth(52);
    background_ = new QCheckBox(QStringLiteral("Sfondo"));
    background_->setObjectName(QStringLiteral("sfondo"));
    backgroundColor_ = new ColorButton(QColor(0, 0, 0, 160), true);
    backgroundColor_->setObjectName(QStringLiteral("coloreSfondo"));
    padding_ = spin(0.0, 20.0, 0.1, 1, QStringLiteral(" %"), "margineSfondo");

    auto* effects = new QWidget;
    auto* effectsGrid = new QGridLayout(effects);
    effectsGrid->setContentsMargins(0, 0, 0, 0);
    effectsGrid->setVerticalSpacing(2);
    effectsGrid->setHorizontalSpacing(6);
    effectsGrid->addWidget(outline_, 0, 0);
    effectsGrid->addWidget(outlineColor_, 0, 1);
    effectsGrid->addWidget(new QLabel(QStringLiteral("spess.")), 0, 2);
    effectsGrid->addWidget(outlineWidth_, 0, 3);
    effectsGrid->addWidget(shadow_, 1, 0);
    effectsGrid->addWidget(shadowColor_, 1, 1);
    effectsGrid->addWidget(new QLabel(QStringLiteral("x / y")), 1, 2);
    auto* shadowOffsets = new QHBoxLayout;
    shadowOffsets->addWidget(shadowX_);
    shadowOffsets->addWidget(shadowY_);
    effectsGrid->addLayout(shadowOffsets, 1, 3);
    effectsGrid->addWidget(background_, 2, 0);
    effectsGrid->addWidget(backgroundColor_, 2, 1);
    effectsGrid->addWidget(new QLabel(QStringLiteral("marg.")), 2, 2);
    effectsGrid->addWidget(padding_, 2, 3);

    // Posizione: nove posti fissi più i valori esatti.
    centerX_ = spin(0.0, 100.0, 1.0, 1, QStringLiteral(" %"), "posizioneX");
    centerY_ = spin(0.0, 100.0, 1.0, 1, QStringLiteral(" %"), "posizioneY");
    length_ = spin(0.1, 36000.0, 0.5, 2, QStringLiteral(" s"), "durata");
    fadeIn_ = spin(0.0, 36000.0, 0.1, 2, QStringLiteral(" s"), "dissolvenzaIn");
    fadeIn_->setToolTip(QStringLiteral("La scritta compare piano piano in questo tempo"));
    fadeOut_ = spin(0.0, 36000.0, 0.1, 2, QStringLiteral(" s"), "dissolvenzaOut");
    fadeOut_->setToolTip(QStringLiteral("La scritta sparisce piano piano in questo tempo"));
    auto* where = new QWidget;
    auto* whereGrid = new QGridLayout(where);
    whereGrid->setContentsMargins(0, 0, 0, 0);
    whereGrid->setVerticalSpacing(2);
    whereGrid->setHorizontalSpacing(6);
    auto* presets = new QGridLayout;
    const char* names[3][3] = {{"alto a sinistra", "in alto", "alto a destra"},
                               {"a sinistra", "al centro", "a destra"},
                               {"basso a sinistra", "in basso", "basso a destra"}};
    const double xs[3] = {0.2, 0.5, 0.8};
    const double ys[3] = {0.12, 0.5, 0.88};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            auto* button = new QToolButton;
            button->setText(QStringLiteral("■"));
            button->setToolTip(QString::fromUtf8(names[row][col]));
            button->setObjectName(QStringLiteral("posto%1%2").arg(row).arg(col));
            button->setFixedSize(20, 18);
            connect(button, &QToolButton::clicked, this, [this, x = xs[col], y = ys[row]] {
                if (onPositionPreset)
                    onPositionPreset(x, y);
                else
                    setPosition(x, y);
            });
            presets->addWidget(button, row, col);
        }
    }
    whereGrid->addLayout(presets, 0, 0, 3, 1);
    // La posizione esatta è nella sezione «Trasforma» (con i rombi): qui restano i nove punti fissi; i campi del centro di stile
    // restano nascosti perché sono il dato dello stile.
    centerX_->setParent(where);
    centerY_->setParent(where);
    centerX_->hide();
    centerY_->hide();
    whereGrid->addWidget(new QLabel(QStringLiteral("Durata")), 0, 1);
    whereGrid->addWidget(length_, 0, 2);
    whereGrid->addWidget(new QLabel(QStringLiteral("Compare")), 1, 1);
    whereGrid->addWidget(fadeIn_, 1, 2);
    whereGrid->addWidget(new QLabel(QStringLiteral("Sparisce")), 2, 1);
    whereGrid->addWidget(fadeOut_, 2, 2);

    controls_ = new QWidget;
    auto* controlsLayout = new QVBoxLayout(controls_);
    controlsLayout_ = controlsLayout;
    controlsLayout->setContentsMargins(6, 4, 6, 4);
    controlsLayout->setSpacing(2);
    {
        // Testo un po' più piccolo in tutta la pagina.
        QFont small = controls_->font();
        small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
        controls_->setFont(small);
    }
    // Sezioni senza riquadro: una riga sottile, il titolo e i campi (come nella pagina delle trasformazioni).
    const auto section = [&](const QString& title, QWidget* content) {
        auto* separator = new QFrame;
        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setEnabled(false);
        controlsLayout->addSpacing(2);
        controlsLayout->addWidget(separator);
        auto* heading = new QLabel(title);
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        controlsLayout->addWidget(heading);
        controlsLayout->addWidget(content);
    };
    controlsLayout->addWidget(text_);
    section(QStringLiteral("Testo"), look);
    section(QStringLiteral("Contorno, ombra e sfondo"), effects);
    section(QStringLiteral("Posizione e durata"), where);
    controlsLayout->addStretch(1);
    auto* scroll = new QScrollArea;
    scroll->setWidget(controls_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // I menu non fanno allargare il pannello: si accorciano.
    for (QComboBox* combo : {static_cast<QComboBox*>(font_), align_, styleList_}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
    }

    hint_ = new QLabel(QStringLiteral("Scegli una scritta sulla linea del tempo (o Scritte > Nuova scritta)."));
    hint_->setObjectName(QStringLiteral("suggerimento"));
    hint_->setWordWrap(true);
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(0, 0, 0, 0);
    main->addWidget(hint_);
    main->addWidget(scroll, 1);

    // Ogni cambiamento arriva subito a onEdited.
    connect(text_, &QPlainTextEdit::textChanged, this, [this] { changed(); });
    connect(font_, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        if (!loading_)
            fontFamily_ = f.family().toStdString();
        changed();
    });
    for (QDoubleSpinBox* box : {size_, lineSpacing_, opacity_, outlineWidth_, shadowX_, shadowY_, padding_, centerX_, centerY_, length_, fadeIn_, fadeOut_})
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    for (QToolButton* button : {bold_, italic_, underline_})
        connect(button, &QToolButton::toggled, this, [this] { changed(); });
    for (QCheckBox* box : {outline_, shadow_, background_})
        connect(box, &QCheckBox::toggled, this, [this] { changed(); });
    connect(align_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    for (ColorButton* button : {color_, outlineColor_, shadowColor_, backgroundColor_})
        button->onColorChanged = [this] { changed(); };
}

void TextEditorPanel::load(const core::TextClip& clip)
{
    loading_ = true;
    const QString text = QString::fromUtf8(clip.text.c_str());
    if (text_->toPlainText() != text)  // uguale = non si tocca, così il cursore resta dov'è
        text_->setPlainText(text);
    length_->setValue(clip.length);
    fadeIn_->setValue(clip.fadeIn);
    fadeOut_->setValue(clip.fadeOut);
    loading_ = false;
    loadStyle(clip.style);
}

void TextEditorPanel::setClip(const core::TextClip* clip, std::vector<core::NamedTextStyle> styles)
{
    const bool stylesSame = styles == styles_;
    styles_ = std::move(styles);
    hasClip_ = clip != nullptr;
    controls_->setEnabled(hasClip_);
    hint_->setVisible(!hasClip_);
    if (!hasClip_)
        return;
    initial_ = *clip;
    if (!stylesSame)
        fillStyleList(std::string());
    // Se i controlli dicono già questa scritta non c'è niente da ricaricare.
    if (!(this->clip() == *clip))
        load(*clip);
}

void TextEditorPanel::focusText()
{
    text_->setFocus();
    text_->selectAll();
}

void TextEditorPanel::loadStyle(const core::TextStyle& s)
{
    loading_ = true;
    fontFamily_ = s.fontFamily;
    font_->setCurrentFont(QFont(QString::fromUtf8(s.fontFamily.c_str())));
    size_->setValue(s.size * 100.0);
    bold_->setChecked(s.bold);
    italic_->setChecked(s.italic);
    underline_->setChecked(s.underline);
    align_->setCurrentIndex(s.align == core::TextAlign::Left ? 0 : s.align == core::TextAlign::Right ? 2 : 1);
    lineSpacing_->setValue(s.lineSpacing);
    color_->setColor(toQt(s.color));
    opacity_->setValue(s.opacity * 100.0);
    outline_->setChecked(s.outline);
    outlineColor_->setColor(toQt(s.outlineColor));
    outlineWidth_->setValue(s.outlineWidth * 100.0);
    shadow_->setChecked(s.shadow);
    shadowColor_->setColor(toQt(s.shadowColor));
    shadowX_->setValue(s.shadowOffsetX * 100.0);
    shadowY_->setValue(s.shadowOffsetY * 100.0);
    background_->setChecked(s.background);
    backgroundColor_->setColor(toQt(s.backgroundColor));
    padding_->setValue(s.backgroundPadding * 100.0);
    centerX_->setValue(s.centerX * 100.0);
    centerY_->setValue(s.centerY * 100.0);
    loading_ = false;
}

core::TextClip TextEditorPanel::clip() const
{
    core::TextClip clip = initial_;
    clip.text = text_->toPlainText().toStdString();
    clip.length = length_->value();
    clip.fadeIn = fadeIn_->value();
    clip.fadeOut = fadeOut_->value();
    clip.style = currentStyle();
    return clip;
}

core::TextStyle TextEditorPanel::currentStyle() const
{
    core::TextStyle style;
    core::TextStyle& s = style;
    s.fontFamily = fontFamily_;
    s.size = size_->value() / 100.0;
    s.bold = bold_->isChecked();
    s.italic = italic_->isChecked();
    s.underline = underline_->isChecked();
    s.align = align_->currentIndex() == 0 ? core::TextAlign::Left : align_->currentIndex() == 2 ? core::TextAlign::Right : core::TextAlign::Center;
    s.lineSpacing = lineSpacing_->value();
    s.color = fromQt(color_->color());
    s.opacity = opacity_->value() / 100.0;
    s.outline = outline_->isChecked();
    s.outlineColor = fromQt(outlineColor_->color());
    s.outlineWidth = outlineWidth_->value() / 100.0;
    s.shadow = shadow_->isChecked();
    s.shadowColor = fromQt(shadowColor_->color());
    s.shadowOffsetX = shadowX_->value() / 100.0;
    s.shadowOffsetY = shadowY_->value() / 100.0;
    s.background = background_->isChecked();
    s.backgroundColor = fromQt(backgroundColor_->color());
    s.backgroundPadding = padding_->value() / 100.0;
    s.centerX = centerX_->value() / 100.0;
    s.centerY = centerY_->value() / 100.0;
    return style;
}

void TextEditorPanel::fillStyleList(const std::string& select)
{
    const QSignalBlocker blocker(styleList_);
    styleList_->clear();
    styleList_->addItem(QStringLiteral("(nessuno)"));
    int selected = 0;
    for (std::size_t i = 0; i < styles_.size(); ++i) {
        styleList_->addItem(QString::fromUtf8(styles_[i].name.c_str()));
        if (styles_[i].name == select)
            selected = static_cast<int>(i) + 1;
    }
    styleList_->setCurrentIndex(selected);
}

void TextEditorPanel::saveStyleAs(const std::string& name)
{
    if (name.empty())
        return;
    const core::TextStyle style = currentStyle();
    const auto found = std::find_if(styles_.begin(), styles_.end(), [&](const core::NamedTextStyle& s) { return s.name == name; });
    if (found != styles_.end())
        found->style = style;
    else
        styles_.push_back({name, style});
    fillStyleList(name);
    if (onStylesChanged)
        onStylesChanged(styles_);
}

void TextEditorPanel::applySelectedStyle()
{
    const int index = styleList_->currentIndex() - 1;
    if (index < 0 || index >= static_cast<int>(styles_.size()))
        return;
    loadStyle(styles_[static_cast<std::size_t>(index)].style);
    changed();
}

void TextEditorPanel::deleteSelectedStyle()
{
    const int index = styleList_->currentIndex() - 1;
    if (index < 0 || index >= static_cast<int>(styles_.size()))
        return;
    styles_.erase(styles_.begin() + index);
    fillStyleList(std::string());
    if (onStylesChanged)
        onStylesChanged(styles_);
}

void TextEditorPanel::setPosition(double x, double y)
{
    centerX_->setValue(x * 100.0);
    centerY_->setValue(y * 100.0);
}

void TextEditorPanel::changed()
{
    if (!loading_ && hasClip_ && onEdited)
        onEdited(clip());
}

}  // namespace editor::app
