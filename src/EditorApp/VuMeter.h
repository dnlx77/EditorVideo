#pragma once

#include "EditorCore/AudioTracks.h"

#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace editor::app {

// Le tacche della scala in dB (le stesse di fader e vu-meter, come nello screenshot di riferimento): i tratti fra una
// tacca e l'altra sono uguali, con interpolazione lineare in mezzo.
inline const std::vector<double>& scaleMarks()
{
    static const std::vector<double> m = {12, 6, 0, -6, -12, -20, -30, -40, -60};
    return m;
}
// Da dB a frazione dell'altezza della guida: 0 in alto (+12), 1 in basso (-60).
inline double dbFraction(double db)
{
    const auto& m = scaleMarks();
    db = std::clamp(db, core::AudioTracks::kMinLevelDb, core::AudioTracks::kMaxLevelDb);
    for (std::size_t i = 0; i + 1 < m.size(); ++i) {
        if (db <= m[i] && db >= m[i + 1])
            return (static_cast<double>(i) + (m[i] - db) / (m[i] - m[i + 1])) / static_cast<double>(m.size() - 1);
    }
    return 1.0;
}
inline double fractionDb(double f)
{
    const auto& m = scaleMarks();
    const double pos = std::clamp(f, 0.0, 1.0) * static_cast<double>(m.size() - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(pos), m.size() - 2);
    return m[i] + (m[i + 1] - m[i]) * (pos - static_cast<double>(i));
}

// Il vu-meter stereo: due barre (sinistra e destra) con la stessa scala del fader, verde fino a -6 dB, giallo fino a 0,
// rosso oltre; una lineetta tiene il picco per un attimo. Il livello scende piano (come i vu-meter veri).
class VuMeter : public QWidget
{
public:
    // Con `showScale` a sinistra delle barre ci sono i numeri della scala in dB (il Master accanto alla linea del tempo).
    explicit VuMeter(QWidget* parent = nullptr, bool showScale = false) : QWidget(parent), showScale_(showScale)
    {
        setFixedWidth(showScale ? 52 : 22);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setToolTip(QStringLiteral("Livello di picco, sinistra e destra"));
    }

    // `peak` è il picco (0..1) dall'ultimo aggiornamento, `seconds` il tempo passato.
    void feed(float left, float right, double seconds)
    {
        const std::array<double, 4> before = {level_[0], level_[1], hold_[0], hold_[1]};
        const std::array<float, 2> in = {left, right};
        for (int c = 0; c < 2; ++c) {
            const double db = in[static_cast<std::size_t>(c)] > 1e-6f ? 20.0 * std::log10(in[static_cast<std::size_t>(c)]) : core::AudioTracks::kMinLevelDb;
            double& level = level_[c];
            level = std::max(db, level - kFallDbPerSecond * seconds);
            level = std::max(level, core::AudioTracks::kMinLevelDb);
            if (level >= hold_[c]) {
                hold_[c] = level;
                holdAge_[c] = 0.0;
            } else {
                holdAge_[c] += seconds;
                if (holdAge_[c] > kHoldSeconds)
                    hold_[c] = std::max(level, hold_[c] - kFallDbPerSecond * seconds);
            }
        }
        // Si ridisegna solo se si vede qualcosa di diverso (a riproduzione ferma i livelli restano in fondo).
        if (level_[0] != before[0] || level_[1] != before[1] || hold_[0] != before[2] || hold_[1] != before[3])
            update();
    }
    void reset()
    {
        level_[0] = level_[1] = hold_[0] = hold_[1] = core::AudioTracks::kMinLevelDb;
        update();
    }
    double levelDb(int channel) const { return level_[channel]; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QPalette& pal = palette();
        const double top = 10;
        const double height = this->height() - 20;
        const double barWidth = 8;
        const double gap = 2;
        const double x0 = showScale_ ? width() - 2 * barWidth - gap - 4 : (width() - 2 * barWidth - gap) / 2.0;
        if (showScale_) {
            p.setPen(pal.color(QPalette::Text));
            QFont f = font();
            f.setPointSizeF(std::max(6.0, f.pointSizeF() - 1.5));
            p.setFont(f);
            const QFontMetricsF fm(f);
            double lastY = -1e9;
            for (const double mark : {12.0, 6.0, 0.0, -6.0, -12.0, -20.0, -30.0, -40.0, -60.0}) {
                const double y = top + dbFraction(mark) * height;
                if (y - lastY < fm.height())
                    continue;  // poca altezza: si saltano le tacche che si toccherebbero
                lastY = y;
                p.drawText(QRectF(0, y - fm.height() / 2, x0 - 4, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                           QString::number(static_cast<int>(mark)));
                p.drawLine(QPointF(x0 - 3, y), QPointF(x0 - 1, y));
            }
        }
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
            fill(core::AudioTracks::kMinLevelDb, -6.0, QColor(46, 204, 64));
            fill(-6.0, 0.0, QColor(240, 196, 25));
            fill(0.0, core::AudioTracks::kMaxLevelDb, QColor(230, 57, 70));
            if (hold_[c] > core::AudioTracks::kMinLevelDb + 0.5) {
                const double y = top + dbFraction(hold_[c]) * height;
                p.fillRect(QRectF(bar.left(), y - 1, barWidth, 2),
                           hold_[c] > 0.0 ? QColor(230, 57, 70) : pal.color(QPalette::Text));
            }
        }
    }

private:
    static constexpr double kFallDbPerSecond = 40.0;
    static constexpr double kHoldSeconds = 1.2;
    double level_[2] = {core::AudioTracks::kMinLevelDb, core::AudioTracks::kMinLevelDb};
    double hold_[2] = {core::AudioTracks::kMinLevelDb, core::AudioTracks::kMinLevelDb};
    double holdAge_[2] = {0.0, 0.0};
    bool showScale_ = false;
};

}  // namespace editor::app
