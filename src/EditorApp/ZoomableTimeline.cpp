#include "EditorApp/ZoomableTimeline.h"

#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <iterator>
#include <cmath>

namespace editor::app {

namespace {

constexpr double kZoomStep = 1.25;          // un "+" o uno scatto di rotellina
constexpr double kPixelsPerFrame = 12.0;    // zoom massimo
constexpr double kWheelScrollFraction = 0.1;  // uno scatto di rotellina scorre di un decimo della larghezza

}  // namespace

ZoomableTimeline::ZoomableTimeline(QWidget* parent)
    : QWidget(parent)
{
    scroll_ = new QScrollBar(Qt::Horizontal, this);
    scroll_->setFocusPolicy(Qt::NoFocus);
    scroll_->hide();
    connect(scroll_, &QScrollBar::valueChanged, this, [this](int value) {
        if (syncing_)
            return;
        zoom_.scrollTo(value / zoom_.pixelsPerSecond());
        userScrolled();
        viewChanged();
    });

    fit_ = new QToolButton(this);
    fit_->setText(QStringLiteral("Adatta"));
    fit_->setToolTip(QStringLiteral("Tutta la linea del tempo nella larghezza (Ctrl+rotellina o + e - per ingrandire e "
                                    "ridurre, anche oltre)"));
    fit_->setFocusPolicy(Qt::NoFocus);
    fit_->setAutoRaise(true);
    connect(fit_, &QToolButton::clicked, this, [this] { fitAll(); });
    syncControls();
}

void ZoomableTimeline::setTimelineDuration(double seconds)
{
    zoom_.setDuration(seconds);
    syncControls();
}

void ZoomableTimeline::setFrameDuration(double seconds)
{
    frameDuration_ = std::max(0.001, seconds > 0 ? seconds : 1.0 / 30);
    zoom_.setMaxPixelsPerSecond(kPixelsPerFrame / frameDuration_);
    syncControls();
    viewChanged();
}

double ZoomableTimeline::xForTime(double seconds) const
{
    return timeAxis().left() + zoom_.xForTime(seconds);
}

double ZoomableTimeline::timeForX(double x) const
{
    return zoom_.timeForX(x - timeAxis().left());
}

void ZoomableTimeline::zoomAtX(double factor, double widgetX)
{
    const bool wasFit = zoom_.isFit();
    zoom_.zoomAt(factor, widgetX - timeAxis().left());
    if (zoom_.isFit() && !wasFit)
        following_ = true;
    syncControls();
    viewChanged();
}

void ZoomableTimeline::zoomIn()
{
    const double x = xForTime(playheadTime());
    const QRectF axis = timeAxis();
    zoomAtX(kZoomStep, x >= axis.left() && x <= axis.right() ? x : axis.center().x());
}

void ZoomableTimeline::zoomOut()
{
    const double x = xForTime(playheadTime());
    const QRectF axis = timeAxis();
    zoomAtX(1.0 / kZoomStep, x >= axis.left() && x <= axis.right() ? x : axis.center().x());
}

void ZoomableTimeline::fitAll()
{
    zoom_.fit();
    following_ = true;
    syncControls();
    viewChanged();
}

void ZoomableTimeline::resumeFollowing()
{
    following_ = true;
}

void ZoomableTimeline::setCacheRange(double from, double to)
{
    if (to <= from)
        from = to = 0.0;
    // Si ridisegna solo se la barretta cambia di almeno mezzo pixel (durante la riproduzione arriva ~30 volte al
    // secondo).
    const auto moved = [&](double a, double b) { return std::abs(xForTime(a) - xForTime(b)) >= 0.5; };
    if (!moved(from, cacheFrom_) && !moved(to, cacheTo_))
        return;
    cacheFrom_ = from;
    cacheTo_ = to;
    update(cacheStrip().toAlignedRect().adjusted(-1, -1, 1, 1));
}

QRectF ZoomableTimeline::cacheStrip() const
{
    const QRectF ruler = rulerRect();
    return QRectF(ruler.left(), ruler.bottom() - 4, ruler.width(), 3);
}

QRectF ZoomableTimeline::rulerRect() const
{
    const QRectF axis = timeAxis();
    return QRectF(axis.left(), 0, axis.width(), kRulerHeight);
}

QString ZoomableTimeline::timecode(double seconds) const
{
    // Secondi interi dal tempo, fotogrammi dal resto: così con 29,97 o 59,94 fps i secondi tondi restano ":00".
    const double fps = 1.0 / frameDuration_;
    const double t = std::max(0.0, seconds) + 1e-6;
    const auto secondsPart = static_cast<long long>(std::floor(t));
    const auto framesPerSecond = std::max<long long>(1, static_cast<long long>(std::ceil(fps - 0.001)));
    const long long frame = std::clamp<long long>(static_cast<long long>(std::floor((t - secondsPart) * fps + 0.001)), 0,
                                                  framesPerSecond - 1);
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secondsPart / 3600, 2, 10, QLatin1Char('0'))
        .arg(secondsPart / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(secondsPart % 60, 2, 10, QLatin1Char('0'))
        .arg(frame, 2, 10, QLatin1Char('0'));
}

void ZoomableTimeline::paintRuler(QPainter& p) const
{
    const QRectF ruler = rulerRect();
    const QPalette pal = palette();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Button));
    p.drawRect(ruler);
    QColor line = pal.color(QPalette::ButtonText);
    line.setAlpha(140);
    p.setPen(QPen(line, 1));
    p.drawLine(QPointF(ruler.left(), ruler.bottom()), QPointF(ruler.right(), ruler.bottom()));
    const double pps = zoom_.pixelsPerSecond();
    if (pps <= 0 || !isEnabled()) {
        p.restore();
        return;
    }

    QFont font = p.font();
    font.setPointSizeF(font.pointSizeF() * 0.75);
    p.setFont(font);
    const double labelWidth = QFontMetricsF(font).horizontalAdvance(QStringLiteral("00:00:00:00")) + 16;

    // Passo fra le etichette: il più piccolo che le lascia distanziate quanto basta; in mezzo tacche più corte.
    const double f = frameDuration_;
    const double steps[] = {f, 2 * f, 5 * f, 10 * f, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 18000, 36000};
    double major = steps[std::size(steps) - 1];
    for (double s : steps) {
        if (s * pps >= labelWidth) {
            major = s;
            break;
        }
    }
    int divisions = 1;
    for (int d : {10, 5, 4, 2}) {
        const double minor = major / d;
        // Sotto il secondo le tacche cadono su fotogrammi interi.
        const bool wholeFrames = std::abs(minor / f - std::round(minor / f)) < 1e-6 && minor >= f - 1e-9;
        if (minor * pps >= 7 && (major >= 1 || wholeFrames)) {
            divisions = d;
            break;
        }
    }
    const double minor = major / divisions;

    const double from = std::max(0.0, timeForX(ruler.left()));
    const double to = timeForX(ruler.right());
    p.setClipRect(ruler);
    const QColor text = pal.color(QPalette::ButtonText);
    for (auto k = static_cast<long long>(std::floor(from / minor)); k * minor <= to + 1e-9; ++k) {
        const double t = k * minor;
        const double x = std::round(xForTime(t)) + 0.5;
        if (k % divisions == 0) {
            p.setPen(QPen(line, 1));
            p.drawLine(QPointF(x, ruler.top() + 3), QPointF(x, ruler.bottom()));
            p.setPen(text);
            p.drawText(QRectF(x + 3, ruler.top() + 1, labelWidth, ruler.height() - 7), Qt::AlignLeft | Qt::AlignVCenter, timecode(t));
        } else {
            p.setPen(QPen(line, 1));
            p.drawLine(QPointF(x, ruler.bottom() - 5), QPointF(x, ruler.bottom()));
        }
    }
    p.restore();
}

void ZoomableTimeline::paintCacheBar(QPainter& p) const
{
    if (cacheTo_ <= cacheFrom_)
        return;
    const QRectF strip = cacheStrip();
    const double left = std::clamp(xForTime(cacheFrom_), strip.left(), strip.right());
    const double right = std::clamp(xForTime(cacheTo_), strip.left(), strip.right());
    if (right - left < 1.0)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(86, 170, 255));  // celeste, si vede sia sul tema chiaro sia su quello scuro
    p.drawRect(QRectF(left, strip.top(), right - left, strip.height()));
    p.restore();
}

void ZoomableTimeline::followPlayhead(double seconds)
{
    if (!following_ || !zoom_.follow(seconds))
        return;
    syncControls();
    viewChanged();
}

void ZoomableTimeline::userScrolled()
{
    following_ = false;
}

void ZoomableTimeline::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    zoom_.setWidth(timeAxis().width());
    // Riga in basso: barra di scorrimento a sinistra, "Adatta" a destra.
    const int buttonWidth = fit_->sizeHint().width();
    const int top = height() - kControlsHeight;
    fit_->setGeometry(width() - buttonWidth - 2, top, buttonWidth, kControlsHeight);
    const int left = static_cast<int>(timeAxis().left());
    scroll_->setGeometry(left, top + 2, std::max(10, width() - buttonWidth - 8 - left), kControlsHeight - 4);
    syncControls();
}

void ZoomableTimeline::syncControls()
{
    syncing_ = true;
    // La barra serve solo quando il contenuto è più largo (ingrandito); rimpicciolito oltre "Adatta" sta tutto.
    const bool scrollable = zoom_.canScroll();
    scroll_->setVisible(scrollable);
    fit_->setEnabled(!zoom_.isFit());
    if (scrollable) {
        const double pps = zoom_.pixelsPerSecond();
        scroll_->setRange(0, static_cast<int>(std::lround(zoom_.maxOffset() * pps)));
        scroll_->setPageStep(static_cast<int>(zoom_.width()));
        scroll_->setSingleStep(std::max(1, static_cast<int>(zoom_.width() * kWheelScrollFraction)));
        scroll_->setValue(static_cast<int>(std::lround(zoom_.offset() * pps)));
    }
    syncing_ = false;
}

void ZoomableTimeline::wheelEvent(QWheelEvent* event)
{
    const QPoint delta = event->angleDelta();
    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl+rotellina: zoom centrato sul mouse.
        if (delta.y() != 0)
            zoomAtX(std::pow(kZoomStep, delta.y() / 120.0), event->position().x());
        event->accept();
        return;
    }
    if (!zoom_.canScroll()) {
        event->ignore();  // niente da scorrere: la rotellina va a chi contiene la linea del tempo
        return;
    }
    // Rotellina (anche con Maiusc, o in orizzontale sul touchpad): scorrimento orizzontale.
    const int steps = delta.x() != 0 ? delta.x() : delta.y();
    zoom_.scrollBy(-steps / 120.0 * zoom_.width() * kWheelScrollFraction);
    userScrolled();
    syncControls();
    viewChanged();
    event->accept();
}

}  // namespace editor::app
