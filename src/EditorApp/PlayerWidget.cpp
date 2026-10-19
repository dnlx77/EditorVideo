#include "EditorApp/PlayerWidget.h"

#include "EditorCore/AudioDecoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Playback.h"
#include "EditorCore/VideoDecoder.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace editor::app {

namespace {

constexpr std::size_t kMaxQueuedFrames = 8;

QString timeText(double seconds)
{
    return QString::fromStdString(core::formatDuration(seconds));
}

}  // namespace

// Area nera in cui viene disegnato il fotogramma corrente, con le proporzioni giuste.
class VideoView : public QWidget
{
public:
    explicit VideoView(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(320, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

    void setFrame(std::shared_ptr<core::VideoFrame> frame)
    {
        frame_ = std::move(frame);
        if (frame_)
            message_.clear();
        update();
    }

    void setMessage(const QString& message)
    {
        frame_.reset();
        message_ = message;
        update();
    }

    // Dimensione dell'area in pixel fisici (con lo zoom di Windows applicato).
    QSize pixelSize() const { return (QSizeF(size()) * devicePixelRatioF()).toSize(); }

    std::function<void()> onResized;

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        if (onResized)
            onResized();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (frame_) {
            // Di solito il decoder produce già fotogrammi della misura giusta e il disegno è 1:1, senza scalare.
            // Subito dopo un ridimensionamento della finestra può arrivare ancora qualche fotogramma della misura
            // precedente: lo adattiamo al volo.
            QImage image(frame_->pixels.data(), frame_->width, frame_->height, frame_->stride, QImage::Format_RGB32);
            const qreal dpr = devicePixelRatioF();
            image.setDevicePixelRatio(dpr);
            const QSizeF logical = QSizeF(frame_->width, frame_->height) / dpr;
            const QSizeF fitted = logical.scaled(QSizeF(size()), Qt::KeepAspectRatio);
            const bool exact = std::abs(fitted.width() - logical.width()) < 1.0
                            && std::abs(fitted.height() - logical.height()) < 1.0;
            const QSizeF drawn = exact ? logical : fitted;
            const QRectF target(QPointF((width() - drawn.width()) / 2, (height() - drawn.height()) / 2), drawn);
            painter.drawImage(target, image);
        } else if (!message_.isEmpty()) {
            painter.setPen(QColor(200, 200, 200));
            painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, message_);
        }
    }

private:
    std::shared_ptr<core::VideoFrame> frame_;
    QString message_;
};

// Barra di posizione: un clic porta subito al punto cliccato (non a scatti di pagina come QSlider normale).
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            const int value = QStyle::sliderValueFromPosition(minimum(), maximum(),
                                                              static_cast<int>(event->position().x()), width());
            setValue(value);
        }
        QSlider::mousePressEvent(event);  // la maniglia ora è sotto il mouse: si può anche trascinare
    }
};

PlayerWidget::PlayerWidget(QWidget* parent)
    : QWidget(parent)
{
    view_ = new VideoView;

    playButton_ = new QToolButton;
    playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playButton_->setToolTip(QStringLiteral("Riproduci (Spazio)"));
    playButton_->setFocusPolicy(Qt::NoFocus);
    connect(playButton_, &QToolButton::clicked, this, [this] { togglePlayPause(); });

    slider_ = new SeekSlider;
    connect(slider_, &QSlider::sliderMoved, this, [this](int value) { updatePositionLabel(value / 1000.0); });
    connect(slider_, &QSlider::sliderReleased, this, [this] { seekTo(slider_->value() / 1000.0); });

    timeLabel_ = new QLabel;
    decoderLabel_ = new QLabel;

    auto* controls = new QHBoxLayout;
    controls->addWidget(playButton_);
    controls->addWidget(timeLabel_);
    controls->addWidget(slider_, 1);
    controls->addWidget(decoderLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_, 1);
    layout->addLayout(controls);

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(5);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });

    // Il decoder produce fotogrammi già della misura dell'anteprima. In pausa, a ridimensionamento finito,
    // ridecodifichiamo il fotogramma corrente alla nuova misura per averlo nitido.
    resizeRefresh_ = new QTimer(this);
    resizeRefresh_->setSingleShot(true);
    resizeRefresh_->setInterval(150);
    connect(resizeRefresh_, &QTimer::timeout, this, [this] {
        if (video_ && !playing_)
            showFrameAt(position_);
    });
    view_->onResized = [this] {
        if (!video_)
            return;
        const QSize pixels = view_->pixelSize();
        video_->setOutputSize(pixels.width(), pixels.height());
        resizeRefresh_->start();
    };

    updateControls();
    updatePositionLabel(0.0);
}

PlayerWidget::~PlayerWidget()
{
    stopWorkers();
}

bool PlayerWidget::hasMedia() const
{
    return video_ != nullptr || audio_ != nullptr;
}

void PlayerWidget::openFile(const QString& path)
{
    closeFile();
    const std::string utf8Path = QDir::toNativeSeparators(path).toStdString();

    QString videoError;
    try {
        video_ = std::make_unique<core::VideoDecoder>(utf8Path);
    } catch (const std::exception& e) {
        videoError = QString::fromUtf8(e.what());
    }
    try {
        audio_ = std::make_unique<core::AudioDecoder>(utf8Path);
    } catch (const std::exception&) {
        // File senza audio: si riproduce solo il video.
    }

    if (!hasMedia()) {
        view_->setMessage(QStringLiteral("Anteprima non disponibile.\n%1").arg(videoError));
        updateControls();
        return;
    }

    audioNote_.clear();
    // EDITORVIDEO_NO_AUDIO=1 simula un PC senza scheda audio (per provare quel caso anche dove la scheda c'è).
    const bool audioDisabled = qEnvironmentVariableIntValue("EDITORVIDEO_NO_AUDIO") != 0;
    if (audio_ && (audioDisabled || !audioOut_.open(audio_->sampleRate(), audio_->channels())))
        audioNote_ = QStringLiteral("audio non disponibile");

    duration_ = std::max({0.0, video_ ? video_->durationSeconds() : 0.0, audio_ ? audio_->durationSeconds() : 0.0});
    slider_->setRange(0, static_cast<int>(std::lround(duration_ * 1000)));
    position_ = 0.0;

    if (video_) {
        const QSize pixels = view_->pixelSize();
        video_->setOutputSize(pixels.width(), pixels.height());
        showFrameAt(0.0);
    } else if (!videoError.isEmpty() && !videoError.contains(QStringLiteral("non contiene un flusso video")))
        view_->setMessage(QStringLiteral("Video non riproducibile, solo audio.\n%1").arg(videoError));
    else
        view_->setMessage(QStringLiteral("Solo audio"));

    updateControls();
    updatePositionLabel(0.0);
}

void PlayerWidget::closeFile()
{
    stopWorkers();
    playing_ = false;
    video_.reset();
    audio_.reset();
    audioOut_.close();
    decoderText_.clear();
    duration_ = 0.0;
    position_ = 0.0;
    slider_->setRange(0, 0);
    view_->setMessage(QString());
    updateControls();
    updatePositionLabel(0.0);
}

void PlayerWidget::togglePlayPause()
{
    if (playing_)
        pause();
    else
        play();
}

void PlayerWidget::play()
{
    if (!hasMedia() || playing_)
        return;
    if (duration_ > 0 && position_ >= duration_ - 0.05)
        position_ = 0.0;  // alla fine: si ricomincia dall'inizio
    startWorkers(position_);
    playing_ = true;
    updateControls();
}

void PlayerWidget::pause()
{
    if (!playing_)
        return;
    position_ = std::clamp(clock(), 0.0, duration_ > 0 ? duration_ : clock());
    stopWorkers();
    playing_ = false;
    updateControls();
    updatePositionLabel(position_);
}

void PlayerWidget::seekTo(double seconds)
{
    if (!hasMedia())
        return;
    const double target = std::clamp(seconds, 0.0, duration_ > 0 ? duration_ : std::max(0.0, seconds));
    const bool wasPlaying = playing_;
    if (wasPlaying)
        stopWorkers();
    position_ = target;
    if (wasPlaying)
        startWorkers(target);
    else if (video_)
        showFrameAt(target);
    updateControls();
    updatePositionLabel(target);
}

void PlayerWidget::seekRelative(double deltaSeconds)
{
    seekTo(currentPosition() + deltaSeconds);
}

double PlayerWidget::currentPosition()
{
    return playing_ ? clock() : position_;
}

QString PlayerWidget::statusText() const
{
    return decoderLabel_->text();
}

void PlayerWidget::startWorkers(double from)
{
    // Il fotogramma di partenza si decodifica qui (a thread fermi), poi il thread video prosegue dal successivo.
    if (video_)
        showFrameAt(from);

    {
        std::lock_guard lock(queueMutex_);
        queue_.clear();
        videoEnded_ = video_ == nullptr;
    }

    audioActive_ = audio_ != nullptr && audioOut_.isOpen();
    audioEnded_ = !audioActive_;
    if (audioActive_) {
        audio_->seek(from);
        audioOut_.reset();
        audioStart_ = from;
    }
    audioClock_ = audioActive_;
    wallBase_ = from;
    wallStart_ = std::chrono::steady_clock::now();

    stopWorkers_ = false;
    if (video_)
        videoThread_ = std::thread([this] { videoWorker(); });
    if (audioActive_)
        audioThread_ = std::thread([this] { audioWorker(); });
    timer_->start();
}

void PlayerWidget::stopWorkers()
{
    timer_->stop();
    stopWorkers_ = true;
    queueSpace_.notify_all();
    if (videoThread_.joinable())
        videoThread_.join();
    if (audioThread_.joinable())
        audioThread_.join();
    audioOut_.reset();

    std::lock_guard lock(queueMutex_);
    queue_.clear();
}

void PlayerWidget::videoWorker()
{
    try {
        while (!stopWorkers_) {
            {
                std::unique_lock lock(queueMutex_);
                queueSpace_.wait(lock, [this] { return stopWorkers_ || queue_.size() < kMaxQueuedFrames; });
                if (stopWorkers_)
                    break;
            }
            auto frame = video_->nextFrame();
            std::lock_guard lock(queueMutex_);
            if (!frame) {
                videoEnded_ = true;
                break;
            }
            queue_.push_back(std::move(frame));
        }
    } catch (const std::exception&) {
        // Errore di decodifica a metà file: il video si ferma sull'ultimo fotogramma, l'audio prosegue.
        std::lock_guard lock(queueMutex_);
        videoEnded_ = true;
    }
}

void PlayerWidget::audioWorker()
{
    try {
        bool first = true;
        while (!stopWorkers_) {
            auto chunk = audio_->next();
            if (!chunk)
                break;
            if (first) {
                audioStart_ = chunk->time;
                first = false;
            }
            if (!audioOut_.write(chunk->samples.data(), chunk->samples.size(), stopWorkers_))
                break;
        }
        if (!stopWorkers_)
            audioOut_.flush();
    } catch (const std::exception&) {
        // Errore di decodifica: l'orologio passa al tempo reale e il video prosegue.
    }
    audioEnded_ = true;
}

double PlayerWidget::clock()
{
    if (audioClock_) {
        const double now = audioStart_.load() + audioOut_.playedFrames() / double(audioOut_.sampleRate());
        if (audioEnded_ && audioOut_.drained()) {
            // Audio finito (o più corto del video): da qui in poi si va avanti con il tempo reale.
            audioClock_ = false;
            wallBase_ = now;
            wallStart_ = std::chrono::steady_clock::now();
        }
        return now;
    }
    return wallBase_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart_).count();
}

void PlayerWidget::tick()
{
    const double now = clock();

    std::shared_ptr<core::VideoFrame> latest;
    bool videoDone = false;
    {
        std::lock_guard lock(queueMutex_);
        // Mostriamo l'ultimo fotogramma il cui tempo è già arrivato; quelli in ritardo vengono saltati.
        while (!queue_.empty() && queue_.front()->time <= now) {
            latest = std::move(queue_.front());
            queue_.pop_front();
        }
        videoDone = videoEnded_ && queue_.empty();
    }
    queueSpace_.notify_one();
    if (latest)
        view_->setFrame(std::move(latest));

    position_ = duration_ > 0 ? std::clamp(now, 0.0, duration_) : std::max(0.0, now);
    updatePositionLabel(position_);

    core::PlaybackState state;
    state.hasVideo = video_ != nullptr;
    state.videoFinished = videoDone;
    state.audioActive = audioActive_;
    state.audioFinished = audioEnded_ && audioOut_.drained();
    state.clock = now;
    state.duration = duration_;
    if (core::playbackFinished(state)) {
        stopWorkers();
        playing_ = false;
        if (duration_ > 0)
            position_ = duration_;
        updateControls();
        updatePositionLabel(position_);
    }
}

void PlayerWidget::showFrameAt(double seconds)
{
    try {
        if (auto frame = video_->seek(seconds))
            view_->setFrame(std::move(frame));
    } catch (const std::exception& e) {
        view_->setMessage(QStringLiteral("Errore di decodifica del video.\n%1").arg(QString::fromUtf8(e.what())));
    }
    // Qui i thread sono fermi: si può leggere in sicurezza quale decoder è in uso.
    decoderText_ = QStringLiteral("Decodifica: %1").arg(QString::fromStdString(video_->decoderDescription()));
    updateControls();
}

void PlayerWidget::updateControls()
{
    const bool media = hasMedia();
    playButton_->setEnabled(media);
    slider_->setEnabled(media && duration_ > 0);
    playButton_->setIcon(style()->standardIcon(playing_ ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    playButton_->setToolTip(playing_ ? QStringLiteral("Pausa (Spazio)") : QStringLiteral("Riproduci (Spazio)"));

    QString decoder;
    if (video_)
        decoder = decoderText_;  // aggiornato da showFrameAt()
    else if (audio_)
        decoder = QStringLiteral("Solo audio");
    if (!audioNote_.isEmpty() && media)
        decoder += QStringLiteral("  ·  %1").arg(audioNote_);
    decoderLabel_->setText(media ? decoder : QString());
}

void PlayerWidget::updatePositionLabel(double seconds)
{
    timeLabel_->setText(QStringLiteral("%1 / %2").arg(timeText(seconds), timeText(duration_)));
    if (!slider_->isSliderDown())
        slider_->setValue(static_cast<int>(std::lround(seconds * 1000)));
}

}  // namespace editor::app
