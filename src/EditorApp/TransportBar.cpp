#include "EditorApp/TransportBar.h"

#include "EditorCore/Playback.h"
#include "EditorCore/Proxy.h"

#include <QtCore/QEvent>
#include <QtGui/QActionGroup>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>

namespace editor::app {

namespace {

// Priorità dei pulsanti: con poco spazio si nascondono prima quelli col numero più basso.
constexpr int kEssential = 100;  // riproduci, fotogramma avanti/indietro
constexpr int kStop = 90;
constexpr int kMarks = 70;       // segno precedente / successivo
constexpr int kInOut = 60;
constexpr int kSpeed = 50;
constexpr int kQuality = 45;
constexpr int kEnds = 40;        // inizio, fine
constexpr int kMute = 35;
constexpr int kLoop = 30;
constexpr int kGoToInOut = 20;
constexpr int kAdd = 15;
constexpr int kVolume = 10;      // cursore del volume

QString speedText(double speed)
{
    QString number = QString::number(std::abs(speed), 'g', 3);
    number.replace(QLatin1Char('.'), QLatin1Char(','));
    return (speed < 0 ? QStringLiteral("◀ ") : QString()) + number + QStringLiteral("×");
}

// Il simbolo `glyph` in un quadrato di `size` pixel logici, nel colore `color`.
QPixmap glyphPixmap(TransportBar::Glyph glyph, const QColor& color, int size, qreal dpr)
{
    using G = TransportBar::Glyph;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    const qreal s = size;
    const auto triangle = [&](qreal left, qreal right, bool pointsRight) {
        QPolygonF t;
        if (pointsRight)
            t << QPointF(left, s * 0.2) << QPointF(right, s * 0.5) << QPointF(left, s * 0.8);
        else
            t << QPointF(right, s * 0.2) << QPointF(left, s * 0.5) << QPointF(right, s * 0.8);
        p.drawPolygon(t);
    };
    const auto bar = [&](qreal x, qreal w) { p.drawRect(QRectF(x, s * 0.2, w, s * 0.6)); };
    switch (glyph) {
    case G::Play:
        triangle(s * 0.28, s * 0.8, true);
        break;
    case G::Pause:
        bar(s * 0.27, s * 0.16);
        bar(s * 0.57, s * 0.16);
        break;
    case G::Stop:
        p.drawRect(QRectF(s * 0.25, s * 0.25, s * 0.5, s * 0.5));
        break;
    case G::SkipBack:
        bar(s * 0.18, s * 0.1);
        triangle(s * 0.28, s * 0.55, false);
        triangle(s * 0.55, s * 0.82, false);
        break;
    case G::SkipForward:
        triangle(s * 0.18, s * 0.45, true);
        triangle(s * 0.45, s * 0.72, true);
        bar(s * 0.72, s * 0.1);
        break;
    case G::SeekBack:
        triangle(s * 0.15, s * 0.5, false);
        triangle(s * 0.5, s * 0.85, false);
        break;
    case G::SeekForward:
        triangle(s * 0.15, s * 0.5, true);
        triangle(s * 0.5, s * 0.85, true);
        break;
    case G::FrameBack:
        triangle(s * 0.22, s * 0.62, false);
        bar(s * 0.68, s * 0.1);
        break;
    case G::FrameForward:
        bar(s * 0.22, s * 0.1);
        triangle(s * 0.38, s * 0.78, true);
        break;
    case G::Volume:
    case G::Muted: {
        QPolygonF speaker;
        speaker << QPointF(s * 0.15, s * 0.38) << QPointF(s * 0.3, s * 0.38) << QPointF(s * 0.5, s * 0.2)
                << QPointF(s * 0.5, s * 0.8) << QPointF(s * 0.3, s * 0.62) << QPointF(s * 0.15, s * 0.62);
        p.drawPolygon(speaker);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, std::max(1.0, s * 0.08), Qt::SolidLine, Qt::RoundCap));
        if (glyph == G::Volume) {
            p.drawArc(QRectF(s * 0.38, s * 0.32, s * 0.3, s * 0.36), -50 * 16, 100 * 16);
            p.drawArc(QRectF(s * 0.36, s * 0.18, s * 0.5, s * 0.64), -50 * 16, 100 * 16);
        } else {
            p.drawLine(QPointF(s * 0.6, s * 0.36), QPointF(s * 0.86, s * 0.64));
            p.drawLine(QPointF(s * 0.86, s * 0.36), QPointF(s * 0.6, s * 0.64));
        }
        break;
    }
    }
    return pixmap;
}

}  // namespace

TransportBar::TransportBar(Kind kind, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    const bool source = kind == Kind::Source;

    const QString mark = source ? QStringLiteral("Fotogramma chiave") : QStringLiteral("Taglio");
    setGlyph(addButton(layout, kEnds, QStringLiteral("Vai all'inizio (Home)"), &onStart), Glyph::SkipBack);
    setGlyph(addButton(layout, kMarks, QStringLiteral("%1 precedente (Maiusc+←)").arg(mark), &onPreviousMark),
             Glyph::SeekBack);
    setGlyph(addButton(layout, kEssential, QStringLiteral("Fotogramma precedente (←)"), &onPreviousFrame),
             Glyph::FrameBack);
    play_ = addButton(layout, kEssential, QString(), &onPlayPause);
    setGlyph(addButton(layout, kStop, QStringLiteral("Stop: ferma e torna dove era partita la riproduzione"), &onStop),
             Glyph::Stop);
    setGlyph(addButton(layout, kEssential, QStringLiteral("Fotogramma successivo (→)"), &onNextFrame),
             Glyph::FrameForward);
    setGlyph(addButton(layout, kMarks, QStringLiteral("%1 successivo (Maiusc+→)").arg(mark), &onNextMark),
             Glyph::SeekForward);
    setGlyph(addButton(layout, kEnds, QStringLiteral("Vai alla fine (Fine)"), &onEnd), Glyph::SkipForward);

    if (source) {
        layout->addSpacing(8);
        addButton(layout, kInOut, QStringLiteral("Segna IN, l'inizio del pezzo (I)"), &onMarkIn)->setText(QStringLiteral("IN"));
        addButton(layout, kInOut, QStringLiteral("Segna OUT, la fine del pezzo (O)"), &onMarkOut)->setText(QStringLiteral("OUT"));
        addButton(layout, kGoToInOut, QStringLiteral("Vai a IN (Maiusc+I)"), &onGoToIn)->setText(QStringLiteral("→IN"));
        addButton(layout, kGoToInOut, QStringLiteral("Vai a OUT (Maiusc+O)"), &onGoToOut)->setText(QStringLiteral("→OUT"));
        addButton(layout, kAdd, QStringLiteral("Aggiungi IN-OUT al montaggio (A)"), &onAdd)
            ->setText(QStringLiteral("+ Montaggio"));
    }

    layout->addSpacing(8);
    infoSlot_ = new QHBoxLayout;
    infoSlot_->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(infoSlot_, 1);

    // Velocità: un menu con le velocità; J e L (spola) la cambiano anche da tastiera.
    speed_ = new QToolButton;
    speed_->setFocusPolicy(Qt::NoFocus);
    speed_->setAutoRaise(true);
    speed_->setPopupMode(QToolButton::InstantPopup);
    speed_->setToolTip(QStringLiteral("Velocità della riproduzione (J / L per la spola, K ferma)"));
    speedMenu_ = new QMenu(speed_);
    auto* speeds = new QActionGroup(speedMenu_);
    for (double s : core::kPlaybackSpeeds) {
        QAction* action = speedMenu_->addAction(speedText(s));
        action->setCheckable(true);
        action->setData(s);
        speeds->addAction(action);
        connect(action, &QAction::triggered, this, [this, s] {
            if (onSpeedChosen)
                onSpeedChosen(s);
        });
    }
    speed_->setMenu(speedMenu_);
    layout->addWidget(speed_);
    items_.push_back({speed_, kSpeed});

    // Qualità dell'anteprima: originale o un proxy più leggero (salti immediati anche con i video pesanti).
    quality_ = new QComboBox;
    quality_->setFocusPolicy(Qt::NoFocus);
    quality_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    quality_->setToolTip(source ? QStringLiteral("Qualità dell'anteprima del file nella Sorgente (ogni file ha la sua): "
                                                 "Originale mostra il file così com'è; 1/2, 1/4 e 1/8 usano un proxy (una "
                                                 "copia più leggera, creata in sottofondo) per muoversi senza attese. "
                                                 "L'esportazione usa sempre gli originali.")
                                : QStringLiteral("Qualità dell'anteprima del file del pezzo mostrato (ogni file ha la sua): "
                                                 "Originale mostra il file così com'è; 1/2, 1/4 e 1/8 usano un proxy (una "
                                                 "copia più leggera, creata in sottofondo) per muoversi senza attese. "
                                                 "L'esportazione usa sempre gli originali."));
    for (const int divisor : core::kPreviewDivisors)
        quality_->addItem(divisor == 1 ? QStringLiteral("Originale") : QStringLiteral("1/%1").arg(divisor), divisor);
    connect(quality_, &QComboBox::activated, this, [this](int index) {
        if (onQualityChosen)
            onQualityChosen(quality_->itemData(index).toInt());
    });
    layout->addWidget(quality_);
    items_.push_back({quality_, kQuality});

    loop_ = new QToolButton;
    loop_->setFocusPolicy(Qt::NoFocus);
    loop_->setAutoRaise(true);
    loop_->setCheckable(true);
    loop_->setText(QStringLiteral("⟲"));
    loop_->setToolTip(source ? QStringLiteral("Ripeti: riproduce di continuo fra IN e OUT (o tutto il file)")
                             : QStringLiteral("Ripeti: riproduce di continuo tutto il montaggio"));
    connect(loop_, &QToolButton::toggled, this, [this](bool on) {
        if (onLoopToggled)
            onLoopToggled(on);
    });
    layout->addWidget(loop_);
    items_.push_back({loop_, kLoop});

    mute_ = new QToolButton;
    mute_->setFocusPolicy(Qt::NoFocus);
    mute_->setAutoRaise(true);
    connect(mute_, &QToolButton::clicked, this, [this] {
        if (onMuteToggled)
            onMuteToggled(!muted_);
    });
    layout->addWidget(mute_);
    items_.push_back({mute_, kMute});

    volume_ = new QSlider(Qt::Horizontal);
    volume_->setRange(0, 100);
    volume_->setFixedWidth(70);
    volume_->setFocusPolicy(Qt::NoFocus);
    volume_->setToolTip(QStringLiteral("Volume dell'anteprima (non cambia il file esportato)"));
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        if (onVolumeChanged)
            onVolumeChanged(value);
    });
    layout->addWidget(volume_);
    items_.push_back({volume_, kVolume});

    setPlaying(false);
    setSpeed(1.0);
    setVolume(100, false);
}

QToolButton* TransportBar::addButton(QHBoxLayout* layout, int priority, const QString& tip,
                                     std::function<void()>* action)
{
    auto* button = new QToolButton;
    button->setFocusPolicy(Qt::NoFocus);  // Spazio e frecce restano alle scorciatoie
    button->setAutoRaise(true);
    button->setToolTip(tip);
    connect(button, &QToolButton::clicked, this, [action] {
        if (*action)
            (*action)();
    });
    layout->addWidget(button);
    items_.push_back({button, priority});
    return button;
}

void TransportBar::setGlyph(QToolButton* button, Glyph glyph)
{
    const auto known = std::find_if(glyphs_.begin(), glyphs_.end(), [button](const auto& g) { return g.first == button; });
    if (known != glyphs_.end()) {
        if (known->second == glyph && !button->icon().isNull())
            return;
        known->second = glyph;
    } else {
        glyphs_.emplace_back(button, glyph);
    }
    // Colore del testo del tema (chiaro sul tema scuro); più tenue se il pulsante è disattivato.
    const QPalette palette = button->palette();
    const int size = button->iconSize().height();
    const qreal dpr = devicePixelRatioF();
    QIcon icon;
    icon.addPixmap(glyphPixmap(glyph, palette.color(QPalette::Active, QPalette::ButtonText), size, dpr), QIcon::Normal);
    icon.addPixmap(glyphPixmap(glyph, palette.color(QPalette::Disabled, QPalette::ButtonText), size, dpr), QIcon::Disabled);
    button->setIcon(icon);
}

void TransportBar::updateIcons()
{
    const auto glyphs = glyphs_;
    for (auto& g : glyphs_)
        g.first->setIcon(QIcon());  // forza il ridisegno
    for (const auto& [button, glyph] : glyphs)
        setGlyph(button, glyph);
}

void TransportBar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    // Tema chiaro/scuro di Windows cambiato, o finestra spostata su un monitor con un'altra scala.
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateIcons();
}

void TransportBar::setInfoWidget(QWidget* widget)
{
    info_ = widget;
    infoSlot_->addWidget(widget, 1);
}

void TransportBar::setMediaEnabled(bool enabled)
{
    for (const Item& item : items_)
        item.widget->setEnabled(enabled);
}

void TransportBar::setPlaying(bool playing)
{
    playing_ = playing;
    setGlyph(play_, playing ? Glyph::Pause : Glyph::Play);
    play_->setToolTip(playing ? QStringLiteral("Pausa (Spazio, K)") : QStringLiteral("Riproduci (Spazio)"));
}

void TransportBar::setSpeed(double speed)
{
    speed_->setText(speedText(speed));
    for (QAction* action : speedMenu_->actions())
        action->setChecked(std::abs(action->data().toDouble() - speed) < 1e-9);
}

void TransportBar::setQuality(int divisor)
{
    const int index = quality_->findData(divisor);
    if (index >= 0 && index != quality_->currentIndex()) {
        const QSignalBlocker block(quality_);
        quality_->setCurrentIndex(index);
    }
}

void TransportBar::setLoop(bool loop)
{
    const QSignalBlocker block(loop_);
    loop_->setChecked(loop);
}

void TransportBar::setVolume(int percent, bool muted)
{
    muted_ = muted;
    {
        const QSignalBlocker block(volume_);
        volume_->setValue(percent);
    }
    setGlyph(mute_, muted || percent == 0 ? Glyph::Muted : Glyph::Volume);
    mute_->setToolTip(muted ? QStringLiteral("Riattiva l'audio dell'anteprima") : QStringLiteral("Togli l'audio dell'anteprima"));
}

QSize TransportBar::minimumSizeHint() const
{
    // Al minimo restano i pulsanti essenziali: la barra non impone una larghezza grande al pannello.
    return {100, QWidget::minimumSizeHint().height()};
}

void TransportBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateVisibility();
}

void TransportBar::updateVisibility()
{
    // Si mostrano i pulsanti in ordine di importanza finché ci stanno, lasciando un po' di posto al tempo. Quelli con
    // la stessa importanza (es. IN e OUT, inizio e fine) si mostrano o nascondono insieme.
    std::vector<int> priorities;
    for (const Item& item : items_)
        priorities.push_back(item.priority);
    std::sort(priorities.begin(), priorities.end(), std::greater<>());
    priorities.erase(std::unique(priorities.begin(), priorities.end()), priorities.end());
    const int reserved = info_ != nullptr ? std::min(info_->sizeHint().width(), 150) : 0;
    int used = reserved + 24;  // spaziature
    bool full = false;
    for (int priority : priorities) {
        int w = 0;
        for (const Item& item : items_) {
            if (item.priority == priority)
                w += item.widget->sizeHint().width() + 1;
        }
        const bool fits = priority >= kEssential || (!full && used + w <= width());
        full = full || !fits;
        for (const Item& item : items_) {
            if (item.priority == priority)
                item.widget->setVisible(fits);
        }
        if (fits)
            used += w;
    }
}

}  // namespace editor::app
