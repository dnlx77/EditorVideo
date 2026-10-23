#include "EditorApp/ZoomableTimeline.h"

#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QToolButton>

#include <algorithm>
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
    zoom_.setMaxPixelsPerSecond(kPixelsPerFrame / std::max(0.001, seconds > 0 ? seconds : 1.0 / 30));
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
    const QRectF axis = timeAxis();
    return QRectF(axis.left(), axis.top() - 5, axis.width(), 3);
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
