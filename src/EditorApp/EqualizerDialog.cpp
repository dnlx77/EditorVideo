#include "EditorApp/EqualizerDialog.h"

#include "EditorApp/UiText.h"
#include "EditorCore/Spectrum.h"

#include <QtCore/QSettings>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace editor::app {

namespace {

constexpr int kSampleRate = 48000;

QColor bandColor(int band)
{
    static const QColor colors[core::kEqBandCount] = {QColor(0xe0, 0x5a, 0x4f), QColor(0xe8, 0x9a, 0x3c), QColor(0xd8, 0xc9, 0x3a),
                                                       QColor(0x5c, 0xc0, 0x6a), QColor(0x4a, 0xa8, 0xe0), QColor(0xa2, 0x7a, 0xe0)};
    return colors[static_cast<std::size_t>(band) % core::kEqBandCount];
}

}  // namespace

// Una manopola: si gira trascinando in verticale (su = più), con la rotella; doppio clic = valore di partenza.
class EqKnob : public QWidget
{
public:
    EqKnob(double minimum, double maximum, double initial, bool logarithmic, QWidget* parent = nullptr)
        : QWidget(parent), min_(minimum), max_(maximum), default_(initial), value_(initial), log_(logarithmic)
    {
        setFixedSize(38, 38);
        setFocusPolicy(Qt::WheelFocus);
    }
    double value() const { return value_; }
    void setValue(double v)
    {
        v = std::clamp(v, min_, max_);
        if (v == value_)
            return;
        value_ = v;
        update();
    }
    void setDefault(double v) { default_ = v; }
    std::function<void(double)> onChanging;
    std::function<void(double)> onCommit;

    // Le azioni dell'utente, per i test.
    void turnTo(double v)
    {
        setValue(v);
        if (onChanging)
            onChanging(value_);
        if (onCommit)
            onCommit(value_);
    }

protected:
    double norm() const
    {
        if (log_)
            return std::log(value_ / min_) / std::log(max_ / min_);
        return (value_ - min_) / (max_ - min_);
    }
    double fromNorm(double n) const
    {
        n = std::clamp(n, 0.0, 1.0);
        return log_ ? min_ * std::pow(max_ / min_, n) : min_ + n * (max_ - min_);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        const QRectF dial = QRectF(rect()).adjusted(4, 4, -4, -4);
        // L'arco del valore.
        p.setPen(QPen(pal.color(QPalette::Mid), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(dial.adjusted(-2, -2, 2, 2), static_cast<int>((90 + 135) * 16), static_cast<int>(-270 * 16));
        p.setPen(QPen(isEnabled() ? QColor(0xe0, 0x7a, 0x30) : pal.color(QPalette::Mid), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(dial.adjusted(-2, -2, 2, 2), static_cast<int>((90 + 135) * 16), static_cast<int>(-270 * norm() * 16));
        p.setPen(QPen(pal.color(QPalette::Shadow), 1));
        p.setBrush(dragging_ ? pal.color(QPalette::Light) : pal.color(QPalette::Button));
        p.drawEllipse(dial.adjusted(3, 3, -3, -3));
        const double angle = (norm() * 270.0 - 135.0) * 3.14159265358979 / 180.0;
        const QPointF c = dial.center();
        const double r = dial.width() / 2.0 - 3;
        p.setPen(QPen(pal.color(QPalette::Text), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(c.x() + std::sin(angle) * r * 0.3, c.y() - std::cos(angle) * r * 0.3),
                   QPointF(c.x() + std::sin(angle) * r * 0.9, c.y() - std::cos(angle) * r * 0.9));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();
        dragging_ = true;
        lastY_ = e->position().y();
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!dragging_)
            return;
        const double dy = lastY_ - e->position().y();
        lastY_ = e->position().y();
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 600.0 : 150.0;
        setValue(fromNorm(norm() + dy / step));
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
        turnTo(default_);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const double delta = e->angleDelta().y();
        if (delta == 0)
            return;
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.005 : 0.02;
        turnTo(fromNorm(norm() + (delta > 0 ? step : -step)));
        e->accept();
    }

private:
    double min_, max_, default_, value_;
    bool log_;
    bool dragging_ = false;
    double lastY_ = 0.0;
};

// Il grafico: la risposta in frequenza (scala logaritmica, +-18 dB) e un punto numerato per banda, da trascinare
// (sinistra-destra = frequenza, su-giù = guadagno); la rotella sul punto cambia la Q.
class EqGraph : public QWidget
{
public:
    explicit EqGraph(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(420, 170);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }
    void setBands(const std::vector<core::EqBand>& bands)
    {
        bands_ = bands;
        update();
    }
    const std::vector<core::EqBand>& bands() const { return bands_; }
    int dragged() const { return drag_; }
    std::function<void(const std::vector<core::EqBand>&, bool final)> onEdited;

    // Lo spettro del suono che passa (core::spectrumDb, bin da sampleRate / kSpectrumSize): sale subito e scende piano.
    // Vuoto = non suona: le barre scendono fino a sparire.
    void setSpectrum(const std::vector<float>& db)
    {
        const QRectF r = plot();
        const int columns = std::max(1, static_cast<int>(r.width()));
        if (static_cast<int>(spectrum_.size()) != columns)
            spectrum_.assign(static_cast<std::size_t>(columns), kSpectrumFloorDb);
        bool any = false;
        for (int x = 0; x < columns; ++x) {
            float target = kSpectrumFloorDb;
            if (!db.empty()) {
                // I bin che cadono nella colonna (in scala logaritmica): il massimo; sotto un bin, l'interpolazione.
                const double hzA = hzOf(r.left() + x);
                const double hzB = hzOf(r.left() + x + 1);
                const double binHz = static_cast<double>(kSampleRate) / static_cast<double>(core::kSpectrumSize);
                const double posA = hzA / binHz;
                const double posB = hzB / binHz;
                const int first = static_cast<int>(std::floor(posA));
                const int last = static_cast<int>(std::floor(posB));
                const int maxIndex = static_cast<int>(db.size()) - 1;
                if (last > first) {
                    for (int i = std::clamp(first, 0, maxIndex); i <= std::clamp(last, 0, maxIndex); ++i)
                        target = std::max(target, db[static_cast<std::size_t>(i)]);
                } else {
                    const double mid = 0.5 * (posA + posB);
                    const int i0 = std::clamp(static_cast<int>(std::floor(mid)), 0, maxIndex);
                    const int i1 = std::min(i0 + 1, maxIndex);
                    const double t = mid - std::floor(mid);
                    target = static_cast<float>(db[static_cast<std::size_t>(i0)] * (1.0 - t) + db[static_cast<std::size_t>(i1)] * t);
                }
            }
            float& shown = spectrum_[static_cast<std::size_t>(x)];
            shown = std::max(target, shown - 3.0f);  // sale subito, scende di 3 dB a ogni aggiornamento
            any = any || shown > kSpectrumFloorDb;
        }
        if (any || spectrumShown_)
            update();
        spectrumShown_ = any;
    }
    bool hasSpectrum() const { return spectrumShown_; }

    // La posizione (in pixel) del punto di una banda: per i test.
    QPointF pointOf(int band) const
    {
        const core::EqBand& b = bands_[static_cast<std::size_t>(band)];
        return QPointF(xOf(b.freq), yOf(b.hasGain() ? b.gainDb : 0.0));
    }

protected:
    QRectF plot() const { return QRectF(rect()).adjusted(34, 8, -10, -8); }
    double xOf(double hz) const
    {
        const QRectF r = plot();
        return r.left() + r.width() * std::log(hz / core::kEqMinHz) / std::log(core::kEqMaxHz / core::kEqMinHz);
    }
    double hzOf(double x) const
    {
        const QRectF r = plot();
        const double n = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
        return core::kEqMinHz * std::pow(core::kEqMaxHz / core::kEqMinHz, n);
    }
    double yOf(double db) const
    {
        const QRectF r = plot();
        return r.center().y() - db / core::kEqMaxGainDb * (r.height() / 2.0);
    }
    double dbOf(double y) const
    {
        const QRectF r = plot();
        return std::clamp((r.center().y() - y) / (r.height() / 2.0) * core::kEqMaxGainDb, -core::kEqMaxGainDb, core::kEqMaxGainDb);
    }
    int hit(const QPointF& pos) const
    {
        int best = -1;
        double bestDist = 14.0 * 14.0;
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            const QPointF c = pointOf(static_cast<int>(i));
            const double d = (c.x() - pos.x()) * (c.x() - pos.x()) + (c.y() - pos.y()) * (c.y() - pos.y());
            if (d <= bestDist) {
                bestDist = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        const QRectF r = plot();
        p.fillRect(r, pal.color(QPalette::Base));
        QFont f = font();
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.5));
        p.setFont(f);
        // Lo spettro del suono, sullo sfondo: da -90 dB (in basso) a 0 dB (in alto).
        if (spectrumShown_ && !spectrum_.empty()) {
            QPainterPath area;
            area.moveTo(r.left(), r.bottom());
            for (std::size_t x = 0; x < spectrum_.size(); ++x) {
                const double n = std::clamp((static_cast<double>(spectrum_[x]) - kSpectrumFloorDb) / (0.0 - kSpectrumFloorDb), 0.0, 1.0);
                area.lineTo(r.left() + static_cast<double>(x) + 0.5, r.bottom() - n * r.height());
            }
            area.lineTo(r.left() + static_cast<double>(spectrum_.size()), r.bottom());
            area.closeSubpath();
            p.setClipRect(r);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x4a, 0xa8, 0xe0, 90));
            p.drawPath(area);
            p.setClipping(false);
        }
        // Griglia: dB ogni 6, frequenze 30 ... 10k.
        for (int db = -18; db <= 18; db += 6) {
            const double y = yOf(db);
            p.setPen(QPen(pal.color(db == 0 ? QPalette::Dark : QPalette::Mid), db == 0 ? 1.5 : 0.5));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(QRectF(0, y - 7, r.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
        }
        const double marks[] = {30, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
        for (const double hz : marks) {
            const double x = xOf(hz);
            p.setPen(QPen(pal.color(QPalette::Mid), 0.5));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(pal.color(QPalette::PlaceholderText));
            const QString text = hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000.0, 0, 'g', 2) : QString::number(hz);
            p.drawText(QRectF(x - 20, r.bottom() - 14, 40, 13), Qt::AlignCenter, text);
        }
        // La curva.
        QPainterPath path;
        const int steps = std::max(2, static_cast<int>(r.width() / 2));
        for (int i = 0; i <= steps; ++i) {
            const double hz = hzOf(r.left() + r.width() * i / steps);
            const double db = std::clamp(core::eqResponseDb(bands_, hz, kSampleRate), -core::kEqMaxGainDb, core::kEqMaxGainDb);
            const QPointF pt(r.left() + r.width() * i / steps, yOf(db));
            if (i == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        p.setClipRect(r);
        p.setPen(QPen(QColor(0xe0, 0x7a, 0x30), 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setClipping(false);
        // I punti.
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            const QPointF c = pointOf(static_cast<int>(i));
            QColor col = bandColor(static_cast<int>(i));
            if (!bands_[i].enabled)
                col = QColor(0x80, 0x80, 0x80);
            p.setPen(QPen(pal.color(QPalette::Shadow), static_cast<int>(i) == drag_ ? 2 : 1));
            p.setBrush(col);
            p.drawEllipse(c, 9, 9);
            p.setPen(Qt::black);
            p.drawText(QRectF(c.x() - 9, c.y() - 9, 18, 18), Qt::AlignCenter, QString::number(i + 1));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        drag_ = hit(e->position());
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (drag_ < 0) {
            setCursor(hit(e->position()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            return;
        }
        core::EqBand& b = bands_[static_cast<std::size_t>(drag_)];
        b.freq = std::clamp(hzOf(e->position().x()), core::kEqMinHz, core::kEqMaxHz);
        if (b.hasGain())
            b.gainDb = std::round(dbOf(e->position().y()) * 2.0) / 2.0;
        update();
        if (onEdited)
            onEdited(bands_, false);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || drag_ < 0)
            return;
        drag_ = -1;
        update();
        if (onEdited)
            onEdited(bands_, true);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const int i = hit(e->position());
        if (i < 0 || e->angleDelta().y() == 0)
            return;
        core::EqBand& b = bands_[static_cast<std::size_t>(i)];
        b.q = std::clamp(b.q * (e->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1), core::kEqMinQ, core::kEqMaxQ);
        update();
        if (onEdited)
            onEdited(bands_, true);
        e->accept();
    }

private:
    static constexpr float kSpectrumFloorDb = -90.0f;
    std::vector<core::EqBand> bands_;
    int drag_ = -1;
    std::vector<float> spectrum_;
    bool spectrumShown_ = false;
};

namespace {

const char* const kTypeKeys[] = {"lc", "ls", "be", "hs", "hc"};

EqualizerDialog::Preset preset(const char* name, std::initializer_list<std::pair<int, core::EqBand>> changes)
{
    EqualizerDialog::Preset p;
    p.name = QString::fromUtf8(name);
    p.bands = core::defaultEqBands();
    for (const auto& [index, band] : changes)
        p.bands[static_cast<std::size_t>(index)] = band;
    return p;
}

core::EqBand band(core::EqBandType type, double freq, double gain, double q, bool enabled = true)
{
    core::EqBand b;
    b.type = type;
    b.freq = freq;
    b.gainDb = gain;
    b.q = q;
    b.enabled = enabled;
    return b;
}

}  // namespace

std::vector<EqualizerDialog::Preset> EqualizerDialog::builtInPresets()
{
    using T = core::EqBandType;
    std::vector<Preset> list;
    list.push_back(preset("Piatto", {}));
    list.push_back(preset("Voce (più chiara)", {{0, band(T::LowCut, 90, 0, 0.71)}, {2, band(T::Bell, 250, -3, 1.0)}, {3, band(T::Bell, 3000, 3, 0.9)},
                                                  {4, band(T::HighShelf, 9000, 2, 0.71)}}));
    list.push_back(preset("Più bassi", {{1, band(T::LowShelf, 110, 5, 0.71)}}));
    list.push_back(preset("Più acuti", {{4, band(T::HighShelf, 7000, 5, 0.71)}}));
    list.push_back(preset("Taglia i bassi", {{0, band(T::LowCut, 120, 0, 0.71)}}));
    list.push_back(preset("Taglia il fruscio", {{5, band(T::HighCut, 9000, 0, 0.71)}}));
    for (Preset& p : list)
        p.builtIn = true;
    return list;
}

QString EqualizerDialog::bandsToText(const std::vector<core::EqBand>& bands)
{
    QStringList parts;
    for (const core::EqBand& b : bands)
        parts << QStringLiteral("%1,%2,%3,%4,%5").arg(QLatin1String(kTypeKeys[static_cast<int>(b.type)])).arg(b.enabled ? 1 : 0)
                     .arg(b.freq, 0, 'g', 8).arg(b.gainDb, 0, 'g', 8).arg(b.q, 0, 'g', 8);
    return parts.join(QLatin1Char(';'));
}

std::vector<core::EqBand> EqualizerDialog::bandsFromText(const QString& text)
{
    std::vector<core::EqBand> bands;
    for (const QString& part : text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList f = part.split(QLatin1Char(','));
        if (f.size() != 5)
            return {};
        core::EqBand b;
        bool found = false;
        for (int i = 0; i < 5; ++i) {
            if (f[0] == QLatin1String(kTypeKeys[i])) {
                b.type = static_cast<core::EqBandType>(i);
                found = true;
            }
        }
        bool ok1 = false, ok2 = false, ok3 = false;
        b.enabled = f[1] == QLatin1String("1");
        b.freq = f[2].toDouble(&ok1);
        b.gainDb = f[3].toDouble(&ok2);
        b.q = f[4].toDouble(&ok3);
        if (!found || !ok1 || !ok2 || !ok3)
            return {};
        bands.push_back(core::clampedBand(b));
    }
    if (bands.size() != core::kEqBandCount)
        return {};
    return bands;
}

std::vector<EqualizerDialog::Preset> EqualizerDialog::userPresets()
{
    std::vector<Preset> list;
    QSettings s = settings();
    s.beginGroup(QStringLiteral("eqPreset"));
    for (const QString& key : s.childKeys()) {
        Preset p;
        p.name = key;
        p.bands = bandsFromText(s.value(key).toString());
        p.builtIn = false;
        if (!p.bands.empty())
            list.push_back(std::move(p));
    }
    return list;
}

void EqualizerDialog::saveUserPreset(const QString& name, const std::vector<core::EqBand>& bands)
{
    QSettings s = settings();
    s.beginGroup(QStringLiteral("eqPreset"));
    s.setValue(name, bandsToText(bands));
}

EqualizerDialog::EqualizerDialog(const QString& title, QWidget* parent) : QDialog(parent), bands_(core::defaultEqBands())
{
    setWindowTitle(title);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    enabled_ = new QCheckBox(QStringLiteral("Attivo"));
    presets_ = new QComboBox;
    presets_->setMinimumWidth(170);
    savePreset_ = new QPushButton(QStringLiteral("Salva preset…"));
    resetAll_ = new QPushButton(QStringLiteral("Azzera tutto"));
    top->addWidget(enabled_);
    top->addSpacing(12);
    top->addWidget(new QLabel(QStringLiteral("Preset:")));
    top->addWidget(presets_);
    top->addWidget(savePreset_);
    top->addStretch(1);
    top->addWidget(resetAll_);
    layout->addLayout(top);

    graph_ = new EqGraph;
    layout->addWidget(graph_, 1);
    graph_->onEdited = [this](const std::vector<core::EqBand>& bands, bool final) {
        bands_ = bands;
        bandsChanged(final);
    };

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(3);
    const char* rowNames[3] = {"Freq", "Gain", "Q"};
    for (int r = 0; r < 3; ++r) {
        auto* label = new QLabel(QString::fromUtf8(rowNames[r]));
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, 3 + 2 * r, 0, 2, 1);
    }
    const std::vector<core::EqBand> start = core::defaultEqBands();
    for (int b = 0; b < static_cast<int>(core::kEqBandCount); ++b) {
        const int col = b + 1;
        auto* header = new QHBoxLayout;
        auto* number = new QLabel(QString::number(b + 1));
        number->setAlignment(Qt::AlignCenter);
        number->setFixedSize(20, 20);
        number->setStyleSheet(QStringLiteral("background: %1; color: black; border-radius: 10px; font-weight: bold;").arg(bandColor(b).name()));
        checks_[static_cast<std::size_t>(b)] = new QCheckBox;
        checks_[static_cast<std::size_t>(b)]->setToolTip(QStringLiteral("Banda attiva"));
        header->addStretch(1);
        header->addWidget(number);
        header->addWidget(checks_[static_cast<std::size_t>(b)]);
        header->addStretch(1);
        grid->addLayout(header, 0, col);
        types_[static_cast<std::size_t>(b)] = new QComboBox;
        for (int t = 0; t < 5; ++t)
            types_[static_cast<std::size_t>(b)]->addItem(QString::fromUtf8(core::bandTypeName(static_cast<core::EqBandType>(t))));
        grid->addWidget(types_[static_cast<std::size_t>(b)], 1, col);
        auto* reset = new QPushButton(QStringLiteral("Reset"));
        reset->setToolTip(QStringLiteral("Riporta la banda ai valori di partenza"));
        grid->addWidget(reset, 2, col);
        connect(reset, &QPushButton::clicked, this, [this, b] { resetBand(b); });

        struct Spec { double lo, hi, init; bool log; int decimals; const char* suffix; double step; };
        const Spec specs[3] = {{core::kEqMinHz, core::kEqMaxHz, start[static_cast<std::size_t>(b)].freq, true, 0, " Hz", 1.0},
                               {-core::kEqMaxGainDb, core::kEqMaxGainDb, 0.0, false, 1, " dB", 0.5},
                               {core::kEqMinQ, core::kEqMaxQ, start[static_cast<std::size_t>(b)].q, true, 2, "", 0.05}};
        for (int w = 0; w < 3; ++w) {
            const std::size_t idx = static_cast<std::size_t>(b * 3 + w);
            auto* knob = new EqKnob(specs[w].lo, specs[w].hi, specs[w].init, specs[w].log);
            auto* spin = new QDoubleSpinBox;
            spin->setRange(specs[w].lo, specs[w].hi);
            spin->setDecimals(specs[w].decimals);
            spin->setSuffix(QString::fromUtf8(specs[w].suffix));
            spin->setSingleStep(specs[w].step);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setAlignment(Qt::AlignCenter);
            spin->setKeyboardTracking(false);
            spin->setMaximumWidth(78);
            knobs_[idx] = knob;
            spins_[idx] = spin;
            grid->addWidget(knob, 3 + 2 * w, col, Qt::AlignHCenter);
            grid->addWidget(spin, 4 + 2 * w, col, Qt::AlignHCenter);
            auto apply = [this, b, w](double v) {
                core::EqBand& band = bands_[static_cast<std::size_t>(b)];
                (w == 0 ? band.freq : w == 1 ? band.gainDb : band.q) = v;
            };
            knob->onChanging = [this, spin, apply](double v) {
                apply(v);
                const QSignalBlocker block(spin);
                spin->setValue(v);
                bandsChanged(false);
            };
            knob->onCommit = [this, spin, apply](double v) {
                apply(v);
                const QSignalBlocker block(spin);
                spin->setValue(v);
                bandsChanged(true);
            };
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, knob, apply](double v) {
                if (updating_)
                    return;
                apply(v);
                knob->setValue(v);
                bandsChanged(true);
            });
        }
        connect(checks_[static_cast<std::size_t>(b)], &QCheckBox::clicked, this, [this, b](bool on) {
            bands_[static_cast<std::size_t>(b)].enabled = on;
            bandsChanged(true);
        });
        connect(types_[static_cast<std::size_t>(b)], &QComboBox::activated, this, [this, b](int t) {
            bands_[static_cast<std::size_t>(b)].type = static_cast<core::EqBandType>(t);
            bandsChanged(true);
        });
    }
    layout->addLayout(grid);

    connect(enabled_, &QCheckBox::clicked, this, [this] { commit(); });
    connect(resetAll_, &QPushButton::clicked, this, [this] {
        bands_ = core::defaultEqBands();
        bandsChanged(true);
    });
    connect(presets_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0 || index > static_cast<int>(presetList_.size()))
            return;
        bands_ = presetList_[static_cast<std::size_t>(index - 1)].bands;
        bandsChanged(true);
    });
    connect(savePreset_, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Salva preset"), QStringLiteral("Nome del preset:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        saveUserPreset(name, bands_);
        fillPresets();
        for (int i = 0; i < static_cast<int>(presetList_.size()); ++i)
            if (presetList_[static_cast<std::size_t>(i)].name == name && !presetList_[static_cast<std::size_t>(i)].builtIn)
                presets_->setCurrentIndex(i + 1);
    });
    fillPresets();
    refreshControls();
}

void EqualizerDialog::setSpectrum(const std::vector<float>& db)
{
    graph_->setSpectrum(db);
}

void EqualizerDialog::turnKnob(int band, int which, double value)
{
    knobs_[static_cast<std::size_t>(band * 3 + which)]->turnTo(value);
}

void EqualizerDialog::fillPresets()
{
    presetList_ = builtInPresets();
    for (Preset& p : userPresets())
        presetList_.push_back(std::move(p));
    presets_->clear();
    presets_->addItem(QStringLiteral("Personalizzato"));
    for (const Preset& p : presetList_)
        presets_->addItem(p.builtIn ? p.name : p.name + QStringLiteral(" *"));
}

void EqualizerDialog::resetBand(int band)
{
    bands_[static_cast<std::size_t>(band)] = core::defaultEqBands()[static_cast<std::size_t>(band)];
    bandsChanged(true);
}

void EqualizerDialog::bandsChanged(bool final)
{
    refreshControls();
    if (final)
        commit();
}

void EqualizerDialog::refreshControls()
{
    updating_ = true;
    graph_->setBands(bands_);
    for (int b = 0; b < static_cast<int>(core::kEqBandCount); ++b) {
        const core::EqBand& band = bands_[static_cast<std::size_t>(b)];
        checks_[static_cast<std::size_t>(b)]->setChecked(band.enabled);
        types_[static_cast<std::size_t>(b)]->setCurrentIndex(static_cast<int>(band.type));
        const double values[3] = {band.freq, band.gainDb, band.q};
        for (int w = 0; w < 3; ++w) {
            const std::size_t idx = static_cast<std::size_t>(b * 3 + w);
            knobs_[idx]->setValue(values[w]);
            const QSignalBlocker block(spins_[idx]);
            spins_[idx]->setValue(values[w]);
            const bool usable = band.enabled && (w != 1 || band.hasGain());
            knobs_[idx]->setEnabled(usable);
            spins_[idx]->setEnabled(usable);
        }
    }
    // Il preset mostrato è quello che coincide con le bande.
    int shown = 0;
    for (int i = 0; i < static_cast<int>(presetList_.size()); ++i)
        if (presetList_[static_cast<std::size_t>(i)].bands == bands_)
            shown = i + 1;
    presets_->setCurrentIndex(shown);
    updating_ = false;
}

void EqualizerDialog::setEffect(const core::AudioEffect& effect)
{
    if (graph_->dragged() >= 0)
        return;  // l'utente sta trascinando un punto
    const core::AudioEffect e = core::clampedEffect(effect);
    enabled_->setChecked(e.enabled);
    bands_ = e.bands;
    refreshControls();
}

core::AudioEffect EqualizerDialog::effect() const
{
    core::AudioEffect e;
    e.kind = core::EffectKind::Equalizer;
    e.enabled = enabled_->isChecked();
    e.bands = bands_;
    return e;
}

void EqualizerDialog::commit()
{
    if (onChanged)
        onChanged(effect());
}

}  // namespace editor::app
