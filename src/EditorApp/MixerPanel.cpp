#include "EditorApp/MixerPanel.h"

#include "EditorCore/AudioTracks.h"

#include <QtCore/QEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

constexpr double kMinDb = core::AudioTracks::kMinLevelDb;
constexpr double kMaxDb = core::AudioTracks::kMaxLevelDb;

// Il fader: una guida verticale con le tacche in dB e il cursore. Trascinando si vede il valore, che si consegna al
// rilascio (un solo passo di Annulla per movimento, come il cursore del livello nella linea del tempo).
class Fader : public QWidget
{
public:
    explicit Fader(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(70, 160);
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
    // Dalla scala in dB alla posizione: i tratti fra le tacche sono uguali (come nello screenshot di riferimento:
    // +12 +6 0 -6 -12 -20 -30 -40 -60), con interpolazione lineare fra una tacca e l'altra.
    static const std::vector<double>& marks()
    {
        static const std::vector<double> m = {12, 6, 0, -6, -12, -20, -30, -40, -60};
        return m;
    }
    double cx() const { return width() * 0.64; }
    QRectF track() const { return QRectF(cx() - 3, 10, 6, height() - 20); }
    double toY(double db) const
    {
        const auto& m = marks();
        const QRectF t = track();
        db = std::clamp(db, kMinDb, kMaxDb);
        for (std::size_t i = 0; i + 1 < m.size(); ++i) {
            if (db <= m[i] && db >= m[i + 1]) {
                const double f = (m[i] - db) / (m[i] - m[i + 1]);
                return t.top() + (static_cast<double>(i) + f) / static_cast<double>(m.size() - 1) * t.height();
            }
        }
        return t.bottom();
    }
    double toDb(double y) const
    {
        const auto& m = marks();
        const QRectF t = track();
        const double pos = std::clamp((y - t.top()) / t.height(), 0.0, 1.0) * static_cast<double>(m.size() - 1);
        const std::size_t i = std::min(static_cast<std::size_t>(pos), m.size() - 2);
        return m[i] + (m[i + 1] - m[i]) * (pos - static_cast<double>(i));
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

}  // namespace

// Una colonna del mixer.
class MixerStrip : public QWidget
{
public:
    MixerStrip(int trackId, const QString& name, QWidget* parent) : QWidget(parent), trackId_(trackId)
    {
        setFixedWidth(96);
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(4, 4, 4, 4);
        col->setSpacing(6);

        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(4);
        solo_ = new QPushButton(QStringLiteral("SOLO"));
        mute_ = new QPushButton(QStringLiteral("MUTE"));
        for (QPushButton* b : {solo_, mute_}) {
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setMinimumWidth(0);
            b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            buttons->addWidget(b);
        }
        solo_->setToolTip(QStringLiteral("Solo: si sentono solo le tracce in solo"));
        mute_->setToolTip(QStringLiteral("Muto: la traccia non si sente"));
        col->addLayout(buttons);

        fader_ = new Fader;
        col->addWidget(fader_, 1);

        readout_ = new QDoubleSpinBox;
        readout_->setRange(kMinDb, kMaxDb);
        readout_->setDecimals(1);
        readout_->setSuffix(QStringLiteral(" dB"));
        readout_->setAlignment(Qt::AlignCenter);
        readout_->setButtonSymbols(QAbstractSpinBox::NoButtons);
        readout_->setKeyboardTracking(false);
        readout_->setToolTip(QStringLiteral("Il livello in dB: si può scrivere"));
        col->addWidget(readout_);

        label_ = new QLabel(name);
        label_->setAlignment(Qt::AlignCenter);
        QFont f = label_->font();
        f.setBold(true);
        label_->setFont(f);
        col->addWidget(label_);
        setObjectName(QStringLiteral("mixerStrip"));
    }

    int trackId() const { return trackId_; }
    QString name() const { return label_->text(); }
    double level() const { return fader_->value(); }
    bool muted() const { return mute_->isChecked(); }
    bool solo() const { return solo_->isChecked(); }
    Fader* fader() const { return fader_; }
    QDoubleSpinBox* readout() const { return readout_; }
    QPushButton* muteButton() const { return mute_; }
    QPushButton* soloButton() const { return solo_; }

    void wire(MixerPanel* panel)
    {
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
        connect(mute_, &QPushButton::clicked, this, [this, panel](bool on) {
            if (panel->onMuted)
                panel->onMuted(trackId_, on);
        });
        connect(solo_, &QPushButton::clicked, this, [this, panel](bool on) {
            if (panel->onSolo)
                panel->onSolo(trackId_, on);
        });
    }

    void setTrack(const MixerPanel::Track& t)
    {
        if (!fader_->dragging()) {
            fader_->setValue(t.levelDb);
            const QSignalBlocker block(readout_);
            if (!readout_->hasFocus())
                readout_->setValue(t.levelDb);
        }
        mute_->setChecked(t.muted);
        solo_->setChecked(t.solo);
        // Una traccia che non si sente (muta, o altre in solo) ha il nome attenuato.
        label_->setEnabled(t.audible);
        fader_->setEnabled(true);
    }

private:
    int trackId_;
    QPushButton* solo_ = nullptr;
    QPushButton* mute_ = nullptr;
    Fader* fader_ = nullptr;
    QDoubleSpinBox* readout_ = nullptr;
    QLabel* label_ = nullptr;
};

MixerPanel::MixerPanel(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    layout_ = new QHBoxLayout(content);
    layout_->setContentsMargins(4, 4, 4, 4);
    layout_->setSpacing(6);
    layout_->addStretch(1);
    area->setWidget(content);
    outer->addWidget(area);
    setMinimumWidth(120);
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
