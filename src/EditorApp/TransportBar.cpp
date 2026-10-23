#include "EditorApp/TransportBar.h"

#include "EditorCore/Playback.h"

#include <QtGui/QActionGroup>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
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

}  // namespace

TransportBar::TransportBar(Kind kind, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    const bool source = kind == Kind::Source;

    addButton(layout, kEnds, QStringLiteral("Vai all'inizio (Home)"), &onStart)
        ->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    addButton(layout, kMarks, QStringLiteral("%1 precedente (Maiusc+←)").arg(source ? QStringLiteral("Fotogramma chiave")
                                                                                    : QStringLiteral("Taglio")),
              &onPreviousMark)
        ->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
    addButton(layout, kEssential, QStringLiteral("Fotogramma precedente (←)"), &onPreviousFrame)
        ->setText(QStringLiteral("◀|"));
    play_ = addButton(layout, kEssential, QString(), &onPlayPause);
    addButton(layout, kStop, QStringLiteral("Stop: ferma e torna dove era partita la riproduzione"), &onStop)
        ->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    addButton(layout, kEssential, QStringLiteral("Fotogramma successivo (→)"), &onNextFrame)
        ->setText(QStringLiteral("|▶"));
    addButton(layout, kMarks, QStringLiteral("%1 successivo (Maiusc+→)").arg(source ? QStringLiteral("Fotogramma chiave")
                                                                                   : QStringLiteral("Taglio")),
              &onNextMark)
        ->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
    addButton(layout, kEnds, QStringLiteral("Vai alla fine (Fine)"), &onEnd)
        ->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));

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
    play_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    play_->setToolTip(playing ? QStringLiteral("Pausa (Spazio, K)") : QStringLiteral("Riproduci (Spazio)"));
}

void TransportBar::setSpeed(double speed)
{
    speed_->setText(speedText(speed));
    for (QAction* action : speedMenu_->actions())
        action->setChecked(std::abs(action->data().toDouble() - speed) < 1e-9);
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
    mute_->setIcon(style()->standardIcon(muted || percent == 0 ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
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
