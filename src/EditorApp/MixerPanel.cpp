#include "EditorApp/MixerPanel.h"

#include "EditorCore/AudioMixer.h"
#include "EditorCore/AudioTracks.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace editor::app {

namespace {

constexpr double kMinDb = core::AudioTracks::kMinLevelDb;
constexpr double kMaxDb = core::AudioTracks::kMaxLevelDb;

// Le tacche della scala in dB (le stesse di fader e vu-meter, come nello screenshot di riferimento): i tratti fra una
// tacca e l'altra sono uguali, con interpolazione lineare in mezzo.
const std::vector<double>& scaleMarks()
{
    static const std::vector<double> m = {12, 6, 0, -6, -12, -20, -30, -40, -60};
    return m;
}
// Da dB a frazione dell'altezza della guida: 0 in alto (+12), 1 in basso (-60).
double dbFraction(double db)
{
    const auto& m = scaleMarks();
    db = std::clamp(db, kMinDb, kMaxDb);
    for (std::size_t i = 0; i + 1 < m.size(); ++i) {
        if (db <= m[i] && db >= m[i + 1])
            return (static_cast<double>(i) + (m[i] - db) / (m[i] - m[i + 1])) / static_cast<double>(m.size() - 1);
    }
    return 1.0;
}
double fractionDb(double f)
{
    const auto& m = scaleMarks();
    const double pos = std::clamp(f, 0.0, 1.0) * static_cast<double>(m.size() - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(pos), m.size() - 2);
    return m[i] + (m[i + 1] - m[i]) * (pos - static_cast<double>(i));
}

// Il fader: una guida verticale con le tacche in dB e il cursore. Trascinando si vede il valore, che si consegna al
// rilascio (un solo passo di Annulla per movimento, come il cursore del livello nella linea del tempo).
class Fader : public QWidget
{
public:
    explicit Fader(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(70, 90);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        setFocusPolicy(Qt::WheelFocus);
        setToolTip(QStringLiteral("Livello della traccia: trascina, rotella per 0,5 dB (Maiusc: 0,1 dB); doppio clic = 0 dB"));
    }

    double value() const { return value_; }
    void setValue(double db)
    {
        db = std::clamp(db, kMinDb, kMaxDb);
        if (db == value_)
            return;
        value_ = db;
        update();
    }
    bool dragging() const { return dragging_; }

    std::function<void(double)> onChanging;  // mentre si trascina
    std::function<void(double)> onCommit;    // il valore definitivo

protected:
    static const std::vector<double>& marks() { return scaleMarks(); }
    double cx() const { return width() * 0.64; }
    QRectF track() const { return QRectF(cx() - 3, 10, 6, height() - 20); }
    double toY(double db) const
    {
        const QRectF t = track();
        return t.top() + dbFraction(db) * t.height();
    }
    double toDb(double y) const
    {
        const QRectF t = track();
        return fractionDb((y - t.top()) / t.height());
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        const QRectF t = track();
        p.setPen(Qt::NoPen);
        p.setBrush(pal.color(QPalette::Dark));
        p.drawRoundedRect(t, 3, 3);
        QFont f = font();
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.5));
        p.setFont(f);
        for (const double mark : marks()) {
            const double y = toY(mark);
            p.setPen(pal.color(QPalette::Mid));
            p.drawLine(QPointF(t.left() - 8, y), QPointF(t.left() - 3, y));
            p.setPen(pal.color(QPalette::PlaceholderText));
            const QString text = mark > 0 ? QStringLiteral("+%1").arg(mark) : QString::number(mark);
            p.drawText(QRectF(0, y - 8, t.left() - 12, 16), Qt::AlignRight | Qt::AlignVCenter, text);
        }
        // Il cursore.
        const double y = toY(value_);
        const QRectF knob(cx() - 14, y - 8, 28, 16);
        p.setPen(QPen(pal.color(QPalette::Shadow), 1));
        p.setBrush(dragging_ || hasFocus() ? pal.color(QPalette::Light) : pal.color(QPalette::Button));
        p.drawRoundedRect(knob, 3, 3);
        p.setPen(pal.color(QPalette::Text));
        p.drawLine(QPointF(knob.left() + 3, y), QPointF(knob.right() - 3, y));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();
        const double y = toY(value_);
        grab_ = std::abs(e->position().y() - y) <= 9 ? e->position().y() - y : 0.0;  // dov'è stato preso il cursore
        dragging_ = true;
        move(e->position().y() - grab_);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (dragging_)
            move(e->position().y() - grab_);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !dragging_)
            return;
        dragging_ = false;
        update();
        if (onCommit)
            onCommit(value_);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        dragging_ = false;
        setValue(0.0);
        if (onChanging)
            onChanging(value_);
        if (onCommit)
            onCommit(value_);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 0.5;
        const double delta = e->angleDelta().y() == 0 ? e->angleDelta().x() : e->angleDelta().y();
        if (delta == 0)
            return;
        setValue(std::round((value_ + (delta > 0 ? step : -step)) * 10.0) / 10.0);
        if (onChanging)
            onChanging(value_);
        if (onCommit)
            onCommit(value_);
        e->accept();
    }

private:
    void move(double y)
    {
        setValue(std::round(toDb(y) * 10.0) / 10.0);
        if (onChanging)
            onChanging(value_);
    }

    double value_ = 0.0;
    double grab_ = 0.0;
    bool dragging_ = false;
};

// La manopola del pan: da S (sinistra) a D (destra), C al centro. Si gira trascinando (su/destra = verso destra), con la
// rotella (Maiusc: passi piccoli); doppio clic = centro. Il valore si consegna al rilascio.
class PanKnob : public QWidget
{
public:
    explicit PanKnob(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(54);
        setMinimumWidth(48);
        setFocusPolicy(Qt::WheelFocus);
        setToolTip(QStringLiteral("Pan: trascina o rotella, doppio clic = centro"));
    }
    double value() const { return value_; }
    bool dragging() const { return dragging_; }
    void setValue(double v)
    {
        v = std::clamp(v, -1.0, 1.0);
        if (v == value_)
            return;
        value_ = v;
        setToolTip(text());
        update();
    }
    QString text() const
    {
        const int percent = static_cast<int>(std::lround(std::abs(value_) * 100.0));
        if (percent == 0)
            return QStringLiteral("Pan: centro");
        return QStringLiteral("Pan: %1% %2").arg(percent).arg(value_ < 0 ? QStringLiteral("sinistra") : QStringLiteral("destra"));
    }
    std::function<void(double)> onChanging;
    std::function<void(double)> onCommit;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        const double d = 34;
        const QRectF dial((width() - d) / 2.0, 2, d, d);
        p.setPen(QPen(pal.color(QPalette::Shadow), 1));
        p.setBrush(dragging_ ? pal.color(QPalette::Light) : pal.color(QPalette::Button));
        p.drawEllipse(dial);
        // Lancetta: 0 = su, ±135 gradi agli estremi.
        const double angle = value_ * 135.0 * 3.14159265358979 / 180.0;
        const QPointF c = dial.center();
        p.setPen(QPen(pal.color(QPalette::Text), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, QPointF(c.x() + std::sin(angle) * d * 0.4, c.y() - std::cos(angle) * d * 0.4));
        QFont f = font();
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.5));
        p.setFont(f);
        p.setPen(pal.color(QPalette::PlaceholderText));
        const double y = dial.bottom() + 1;
        p.drawText(QRectF(dial.left() - 14, y - 8, 14, 14), Qt::AlignCenter, QStringLiteral("S"));
        p.drawText(QRectF(dial.right(), y - 8, 14, 14), Qt::AlignCenter, QStringLiteral("D"));
        p.drawText(QRectF(c.x() - 7, y, 14, 14), Qt::AlignCenter, QStringLiteral("C"));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();
        dragging_ = true;
        lastX_ = e->position().x();
        lastY_ = e->position().y();
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!dragging_)
            return;
        const double delta = (e->position().x() - lastX_) - (e->position().y() - lastY_);
        lastX_ = e->position().x();
        lastY_ = e->position().y();
        setValue(std::round((value_ + delta / 80.0) * 100.0) / 100.0);
        if (onChanging)
            onChanging(value_);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !dragging_)
            return;
        dragging_ = false;
        update();
        if (onCommit)
            onCommit(value_);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        dragging_ = false;
        setValue(0.0);
        if (onChanging)
            onChanging(value_);
        if (onCommit)
            onCommit(value_);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.01 : 0.05;
        const double delta = e->angleDelta().y() == 0 ? e->angleDelta().x() : e->angleDelta().y();
        if (delta == 0)
            return;
        setValue(std::round((value_ + (delta > 0 ? step : -step)) * 100.0) / 100.0);
        if (onChanging)
            onChanging(value_);
        if (onCommit)
            onCommit(value_);
        e->accept();
    }

private:
    double value_ = 0.0;
    double lastX_ = 0.0;
    double lastY_ = 0.0;
    bool dragging_ = false;
};

// Il vu-meter stereo: due barre (sinistra e destra) con la stessa scala del fader, verde fino a -6 dB, giallo fino a 0,
// rosso oltre; una lineetta tiene il picco per un attimo. Il livello scende piano (come i vu-meter veri).
class Meter : public QWidget
{
public:
    explicit Meter(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedWidth(22);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setToolTip(QStringLiteral("Livello di picco della traccia, sinistra e destra"));
    }

    // `peak` è il picco (0..1) dall'ultimo aggiornamento, `seconds` il tempo passato.
    void feed(float left, float right, double seconds)
    {
        const std::array<float, 2> in = {left, right};
        for (int c = 0; c < 2; ++c) {
            const double db = in[static_cast<std::size_t>(c)] > 1e-6f ? 20.0 * std::log10(in[static_cast<std::size_t>(c)]) : kMinDb;
            double& level = level_[c];
            level = std::max(db, level - kFallDbPerSecond * seconds);
            level = std::max(level, kMinDb);
            if (level >= hold_[c]) {
                hold_[c] = level;
                holdAge_[c] = 0.0;
            } else {
                holdAge_[c] += seconds;
                if (holdAge_[c] > kHoldSeconds)
                    hold_[c] = std::max(level, hold_[c] - kFallDbPerSecond * seconds);
            }
        }
        update();
    }
    void reset()
    {
        level_[0] = level_[1] = hold_[0] = hold_[1] = kMinDb;
        update();
    }
    double levelDb(int channel) const { return level_[channel]; }
    double holdDb(int channel) const { return hold_[channel]; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QPalette& pal = palette();
        const double top = 10;
        const double height = this->height() - 20;
        const double barWidth = 8;
        const double gap = 2;
        const double x0 = (width() - 2 * barWidth - gap) / 2.0;
        for (int c = 0; c < 2; ++c) {
            const QRectF bar(x0 + c * (barWidth + gap), top, barWidth, height);
            p.fillRect(bar, pal.color(QPalette::Dark));
            const auto fill = [&](double fromDb, double toDb, const QColor& color) {
                const double hi = std::min(toDb, level_[c]);
                if (hi <= fromDb)
                    return;
                const double y1 = top + dbFraction(hi) * height;
                const double y2 = top + dbFraction(fromDb) * height;
                p.fillRect(QRectF(bar.left(), y1, barWidth, y2 - y1), color);
            };
            fill(kMinDb, -6.0, QColor(46, 204, 64));
            fill(-6.0, 0.0, QColor(240, 196, 25));
            fill(0.0, kMaxDb, QColor(230, 57, 70));
            if (hold_[c] > kMinDb + 0.5) {
                const double y = top + dbFraction(hold_[c]) * height;
                p.fillRect(QRectF(bar.left(), y - 1, barWidth, 2),
                           hold_[c] > 0.0 ? QColor(230, 57, 70) : pal.color(QPalette::Text));
            }
        }
    }

private:
    static constexpr double kFallDbPerSecond = 40.0;
    static constexpr double kHoldSeconds = 1.2;
    double level_[2] = {kMinDb, kMinDb};
    double hold_[2] = {kMinDb, kMinDb};
    double holdAge_[2] = {0.0, 0.0};
};

// La finestra di un equalizzatore: tre cursori (bassi, medi, alti) in dB. Si consegna il valore al rilascio di un cursore
// (un solo passo di Annulla per movimento).
class EqualizerDialog : public QDialog
{
public:
    explicit EqualizerDialog(const QString& title, QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(title);
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        auto* layout = new QVBoxLayout(this);
        enabled_ = new QCheckBox(QStringLiteral("Attivo"));
        layout->addWidget(enabled_);
        const char* names[3] = {"Bassi (120 Hz)", "Medi (1 kHz)", "Alti (6 kHz)"};
        for (int i = 0; i < 3; ++i) {
            auto* row = new QHBoxLayout;
            auto* name = new QLabel(QString::fromUtf8(names[i]));
            name->setMinimumWidth(100);
            sliders_[i] = new QSlider(Qt::Horizontal);
            sliders_[i]->setRange(static_cast<int>(core::kEffectMinDb * 2), static_cast<int>(core::kEffectMaxDb * 2));  // passi di 0,5 dB
            sliders_[i]->setMinimumWidth(180);
            values_[i] = new QLabel;
            values_[i]->setMinimumWidth(56);
            values_[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(name);
            row->addWidget(sliders_[i], 1);
            row->addWidget(values_[i]);
            layout->addLayout(row);
            connect(sliders_[i], &QSlider::valueChanged, this, [this, i](int v) {
                values_[i]->setText(QStringLiteral("%1 dB").arg(v / 2.0, 0, 'f', 1));
                if (!sliders_[i]->isSliderDown() && !updating_)
                    commit();  // rotella o tastiera: subito
            });
            connect(sliders_[i], &QSlider::sliderReleased, this, [this] { commit(); });
        }
        auto* reset = new QPushButton(QStringLiteral("Azzera"));
        layout->addWidget(reset, 0, Qt::AlignRight);
        connect(reset, &QPushButton::clicked, this, [this] {
            updating_ = true;
            for (QSlider* s : sliders_)
                s->setValue(0);
            updating_ = false;
            commit();
        });
        connect(enabled_, &QCheckBox::clicked, this, [this] { commit(); });
    }

    void setEffect(const core::AudioEffect& e)
    {
        updating_ = true;
        enabled_->setChecked(e.enabled);
        for (int i = 0; i < 3; ++i) {
            if (!sliders_[i]->isSliderDown())
                sliders_[i]->setValue(static_cast<int>(std::lround(e.params[static_cast<std::size_t>(i)] * 2.0)));
        }
        updating_ = false;
    }
    core::AudioEffect effect() const
    {
        core::AudioEffect e;
        e.kind = core::EffectKind::Equalizer;
        e.enabled = enabled_->isChecked();
        for (int i = 0; i < 3; ++i)
            e.params[static_cast<std::size_t>(i)] = sliders_[i]->value() / 2.0;
        return e;
    }
    std::function<void(const core::AudioEffect&)> onChanged;
    QSlider* slider(int i) const { return sliders_[i]; }

private:
    void commit()
    {
        if (onChanged)
            onChanged(effect());
    }
    QCheckBox* enabled_ = nullptr;
    QSlider* sliders_[3] = {};
    QLabel* values_[3] = {};
    bool updating_ = false;
};

}  // namespace

// Una colonna del mixer.
class MixerStrip : public QWidget
{
public:
    MixerStrip(int trackId, const QString& name, QWidget* parent, bool master = false) : QWidget(parent), trackId_(trackId)
    {
        setFixedWidth(104);
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(4, 4, 4, 4);
        col->setSpacing(6);

        // Gli slot degli effetti, in cima: al più due righe (effetti o "+"), sempre con lo stesso spazio (i fader restano allineati).
        slotBox_ = new QWidget;
        slotBox_->setFixedHeight(2 * 24 + 3);
        slotLayout_ = new QVBoxLayout(slotBox_);
        slotLayout_->setContentsMargins(0, 0, 0, 0);
        slotLayout_->setSpacing(3);
        col->addWidget(slotBox_);

        pan_ = new PanKnob;
        col->addWidget(pan_);

        auto* buttons = new QVBoxLayout;  // uno sopra l'altro: affiancati, in 100 px le scritte si tagliano
        buttons->setSpacing(4);
        solo_ = new QPushButton(QStringLiteral("SOLO"));
        mute_ = new QPushButton(QStringLiteral("MUTE"));
        for (QPushButton* b : {solo_, mute_}) {
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            buttons->addWidget(b);
        }
        solo_->setToolTip(QStringLiteral("Solo: si sentono solo le tracce in solo"));
        mute_->setToolTip(QStringLiteral("Muto: la traccia non si sente"));
        col->addLayout(buttons);

        fader_ = new Fader;
        meter_ = new Meter;
        auto* faderRow = new QHBoxLayout;
        faderRow->setSpacing(0);
        faderRow->addWidget(fader_, 1);
        faderRow->addWidget(meter_);
        col->addLayout(faderRow, 1);

        readout_ = new QDoubleSpinBox;
        readout_->setRange(kMinDb, kMaxDb);
        readout_->setDecimals(1);
        readout_->setSuffix(QStringLiteral(" dB"));
        readout_->setAlignment(Qt::AlignCenter);
        readout_->setButtonSymbols(QAbstractSpinBox::NoButtons);
        readout_->setKeyboardTracking(false);
        readout_->setToolTip(QStringLiteral("Il livello in dB: si può scrivere"));
        col->addWidget(readout_);

        if (master) {
            // Il Master non ha pan, muto e solo: i posti restano (il fader sta alla stessa altezza delle altre colonne).
            for (QWidget* w : std::initializer_list<QWidget*>{pan_, solo_, mute_}) {
                QSizePolicy policy = w->sizePolicy();
                policy.setRetainSizeWhenHidden(true);
                w->setSizePolicy(policy);
                w->hide();
            }
        }
        label_ = new QLabel(name);
        label_->setAlignment(Qt::AlignCenter);
        QFont f = label_->font();
        f.setBold(true);
        label_->setFont(f);
        col->addWidget(label_);
        setObjectName(QStringLiteral("mixerStrip"));
    }

    int trackId() const { return trackId_; }
    // Bordo intorno alla colonna.
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }
    QString name() const { return label_->text(); }
    double level() const { return fader_->value(); }
    bool muted() const { return mute_->isChecked(); }
    bool solo() const { return solo_->isChecked(); }
    Fader* fader() const { return fader_; }
    Meter* meter() const { return meter_; }
    PanKnob* pan() const { return pan_; }
    QDoubleSpinBox* readout() const { return readout_; }
    QPushButton* muteButton() const { return mute_; }
    QPushButton* soloButton() const { return solo_; }

    const std::vector<core::AudioEffect>& effects() const { return effects_; }
    QWidget* slotBox() const { return slotBox_; }
    EqualizerDialog* dialog() const { return dialog_; }
    void openEqualizer()
    {
        const auto it = std::find_if(effects_.begin(), effects_.end(),
                                     [](const core::AudioEffect& e) { return e.kind == core::EffectKind::Equalizer; });
        if (it == effects_.end())
            return;
        if (!dialog_) {
            dialog_ = new EqualizerDialog(QStringLiteral("Equalizzatore - %1").arg(label_->text()), window());
            dialog_->setAttribute(Qt::WA_DeleteOnClose);
            dialog_->onChanged = [this](const core::AudioEffect& e) { changeEffect(core::EffectKind::Equalizer, e); };
        }
        dialog_->setEffect(*it);
        dialog_->show();
        dialog_->raise();
    }

    void wire(MixerPanel* panel)
    {
        panel_ = panel;
        fader_->onChanging = [this](double db) {
            const QSignalBlocker block(readout_);
            readout_->setValue(db);
        };
        fader_->onCommit = [this, panel](double db) {
            if (panel->onLevel)
                panel->onLevel(trackId_, db);
        };
        connect(readout_, &QDoubleSpinBox::valueChanged, this, [this, panel](double db) {
            fader_->setValue(db);
            if (panel->onLevel)
                panel->onLevel(trackId_, db);
        });
        pan_->onCommit = [this, panel](double pan) {
            if (panel->onPan)
                panel->onPan(trackId_, pan);
        };
        connect(mute_, &QPushButton::clicked, this, [this, panel](bool on) {
            if (panel->onMuted)
                panel->onMuted(trackId_, on);
        });
        connect(solo_, &QPushButton::clicked, this, [this, panel](bool on) {
            if (panel->onSolo)
                panel->onSolo(trackId_, on);
        });
    }

    // Cambia (o aggiunge) l'effetto di quel tipo e lo comunica.
    void changeEffect(core::EffectKind kind, const core::AudioEffect& e)
    {
        std::vector<core::AudioEffect> list = effects_;
        const auto it = std::find_if(list.begin(), list.end(), [&](const core::AudioEffect& x) { return x.kind == kind; });
        if (it != list.end())
            *it = e;
        else
            list.push_back(e);
        emitEffects(std::move(list));
    }
    void emitEffects(std::vector<core::AudioEffect> list)
    {
        if (panel_ != nullptr && panel_->onEffects)
            panel_->onEffects(trackId_, std::move(list));
    }

    // Rifà i pulsanti degli slot.
    void rebuildSlots()
    {
        while (QLayoutItem* item = slotLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (const core::AudioEffect& e : effects_) {
            auto* b = new QPushButton(QString::fromUtf8(core::effectName(e.kind)));
            b->setFixedHeight(24);
            b->setFocusPolicy(Qt::NoFocus);
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            b->setToolTip(QStringLiteral("Clic per i parametri, clic destro per escludere o togliere"));
            // Arancione se agisce, come nel mixer di riferimento; grigio se escluso.
            b->setStyleSheet(e.enabled ? QStringLiteral("QPushButton { background: #b4531c; color: white; border: 1px solid #7a3510; border-radius: 3px; }"
                                                        "QPushButton:hover { background: #cc6122; }")
                                       : QStringLiteral("QPushButton { color: gray; font-style: italic; }"));
            const core::EffectKind kind = e.kind;
            connect(b, &QPushButton::clicked, this, [this] { openEqualizer(); });
            connect(b, &QPushButton::customContextMenuRequested, this, [this, kind, b](const QPoint& pos) {
                QMenu menu;
                const auto it = std::find_if(effects_.begin(), effects_.end(), [&](const core::AudioEffect& x) { return x.kind == kind; });
                if (it == effects_.end())
                    return;
                QAction* toggle = menu.addAction(it->enabled ? QStringLiteral("Escludi") : QStringLiteral("Includi"));
                QAction* remove = menu.addAction(QStringLiteral("Togli"));
                QAction* chosen = menu.exec(b->mapToGlobal(pos));
                if (chosen == toggle) {
                    core::AudioEffect changed = *it;
                    changed.enabled = !changed.enabled;
                    changeEffect(kind, changed);
                } else if (chosen == remove) {
                    std::vector<core::AudioEffect> list = effects_;
                    std::erase_if(list, [&](const core::AudioEffect& x) { return x.kind == kind; });
                    emitEffects(std::move(list));
                }
            });
            slotLayout_->addWidget(b);
        }
        // "+": gli effetti che ancora non ci sono.
        const bool hasEqualizer = std::any_of(effects_.begin(), effects_.end(),
                                              [](const core::AudioEffect& e) { return e.kind == core::EffectKind::Equalizer; });
        if (!hasEqualizer && effects_.size() < 2) {
            auto* plus = new QPushButton(QStringLiteral("+"));
            plus->setFixedHeight(24);
            plus->setFocusPolicy(Qt::NoFocus);
            plus->setToolTip(QStringLiteral("Aggiungi un effetto"));
            connect(plus, &QPushButton::clicked, this, [this, plus] {
                QMenu menu;
                QAction* eq = menu.addAction(QString::fromUtf8(core::effectName(core::EffectKind::Equalizer)));
                if (menu.exec(plus->mapToGlobal(QPoint(0, plus->height()))) == eq)
                    changeEffect(core::EffectKind::Equalizer, core::AudioEffect{});
            });
            slotLayout_->addWidget(plus);
        }
        slotLayout_->addStretch(1);
    }

    void setTrack(const MixerPanel::Track& t)
    {
        if (t.effects != effects_ || slotLayout_->count() == 0) {
            effects_ = t.effects;
            rebuildSlots();
            if (dialog_) {
                const auto it = std::find_if(effects_.begin(), effects_.end(),
                                             [](const core::AudioEffect& e) { return e.kind == core::EffectKind::Equalizer; });
                if (it != effects_.end())
                    dialog_->setEffect(*it);
                else
                    dialog_->close();
            }
        }
        if (!fader_->dragging()) {
            fader_->setValue(t.levelDb);
            const QSignalBlocker block(readout_);
            if (!readout_->hasFocus())
                readout_->setValue(t.levelDb);
        }
        if (!pan_->dragging())
            pan_->setValue(t.pan);
        mute_->setChecked(t.muted);
        solo_->setChecked(t.solo);
        // Una traccia che non si sente (muta, o altre in solo) ha il nome attenuato.
        label_->setEnabled(t.audible);
        fader_->setEnabled(true);
    }

private:
    int trackId_;
    MixerPanel* panel_ = nullptr;
    std::vector<core::AudioEffect> effects_;
    QWidget* slotBox_ = nullptr;
    QVBoxLayout* slotLayout_ = nullptr;
    QPointer<EqualizerDialog> dialog_;
    QPushButton* solo_ = nullptr;
    QPushButton* mute_ = nullptr;
    Fader* fader_ = nullptr;
    Meter* meter_ = nullptr;
    PanKnob* pan_ = nullptr;
    QDoubleSpinBox* readout_ = nullptr;
    QLabel* label_ = nullptr;
};

MixerPanel::MixerPanel(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    layout_ = new QHBoxLayout(content);
    layout_->setContentsMargins(4, 4, 4, 4);
    layout_->setSpacing(6);
    layout_->addStretch(1);
    area->setWidget(content);
    outer->addWidget(area, 1);
    // Il Master sta a destra, fuori dall'elenco che scorre: si vede sempre.
    // Come le altre colonne sta in un'area che scorre (in verticale): senza, la sua altezza minima impedirebbe di
    // restringere il pannello e, agganciato accanto alle altre schede, toglierebbe spazio alla linea del tempo.
    auto* masterArea = new QScrollArea;
    masterArea->setWidgetResizable(true);
    masterArea->setFrameShape(QFrame::NoFrame);
    masterArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* masterBox = new QWidget;
    auto* masterLayout = new QVBoxLayout(masterBox);
    masterLayout->setContentsMargins(0, 4, 4, 4);
    master_ = new MixerStrip(core::kMasterTrackId, QStringLiteral("Master"), masterBox, true);
    master_->wire(this);
    masterLayout->addWidget(master_);
    masterArea->setWidget(masterBox);
    masterArea->setFixedWidth(104 + 8 + masterArea->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
    outer->addWidget(masterArea);
    setMinimumWidth(240);
}

void MixerPanel::setMaster(double levelDb, const std::vector<core::AudioEffect>& effects)
{
    Track t;
    t.effects = effects;
    t.trackId = core::kMasterTrackId;
    t.name = QStringLiteral("Master");
    t.levelDb = levelDb;
    master_->setTrack(t);
}

double MixerPanel::masterLevelDb() const
{
    return master_->level();
}

double MixerPanel::masterMeterDb(int channel) const
{
    return master_->meter()->levelDb(channel);
}

void MixerPanel::feedPeaks(const std::map<int, std::array<float, 2>>& peaks, double seconds)
{
    {
        const auto it = peaks.find(core::kMasterTrackId);
        if (it != peaks.end())
            master_->meter()->feed(it->second[0], it->second[1], seconds);
        else
            master_->meter()->feed(0.0f, 0.0f, seconds);
    }
    for (MixerStrip* s : strips_) {
        const auto it = peaks.find(s->trackId());
        if (it != peaks.end())
            s->meter()->feed(it->second[0], it->second[1], seconds);
        else
            s->meter()->feed(0.0f, 0.0f, seconds);
    }
}

double MixerPanel::meterDb(int index, int channel) const
{
    return strips_[static_cast<std::size_t>(index)]->meter()->levelDb(channel);
}

void MixerPanel::resetMeters()
{
    master_->meter()->reset();
    for (MixerStrip* s : strips_)
        s->meter()->reset();
}

void MixerPanel::setTracks(const std::vector<Track>& tracks)
{
    bool same = tracks.size() == tracks_.size();
    for (std::size_t i = 0; same && i < tracks.size(); ++i)
        same = tracks[i].trackId == tracks_[i].trackId && tracks[i].name == tracks_[i].name;
    if (!same) {
        for (MixerStrip* s : strips_)
            delete s;
        strips_.clear();
        for (const Track& t : tracks) {
            auto* s = new MixerStrip(t.trackId, t.name, layout_->parentWidget());
            s->wire(this);
            layout_->insertWidget(static_cast<int>(strips_.size()), s);
            strips_.push_back(s);
        }
    }
    tracks_ = tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        strips_[i]->setTrack(tracks[i]);
}

}  // namespace editor::app
