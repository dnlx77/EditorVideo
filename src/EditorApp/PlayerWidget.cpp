#include "EditorApp/PlayerWidget.h"

#include "EditorApp/TimelineWidget.h"
#include "EditorApp/TransportBar.h"
#include "EditorApp/UiText.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/Playback.h"
#include "EditorCore/TimeStretch.h"
#include "EditorCore/VideoDecoder.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace editor::app {

namespace {

constexpr std::size_t kMaxQueuedFrames = 8;
// All'indietro si decodifica in avanti un blocco alla volta, poi lo si mostra al contrario: al massimo questi
// fotogrammi (sono immagini grandi) o questi secondi per blocco.
constexpr std::size_t kReverseBlockFrames = 24;
constexpr double kReverseBlockSeconds = 1.0;

}  // namespace

// Area nera in cui viene disegnato il fotogramma corrente, con le proporzioni giuste.
class VideoView : public QWidget
{
public:
    explicit VideoView(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(64, 36);
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

    void setBlank(bool blank)
    {
        if (blank != blank_) {
            blank_ = blank;
            update();
        }
    }

    // Dimensione dell'area in pixel fisici (con lo zoom di Windows applicato).
    QSize pixelSize() const { return (QSizeF(size()) * devicePixelRatioF()).toSize(); }

    std::function<void()> onResized;
    std::function<void()> onDoubleClicked;

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && onDoubleClicked)
            onDoubleClicked();
    }

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
        if (blank_)
            return;
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
    bool blank_ = false;
};

PlayerWidget::PlayerWidget(QWidget* parent)
    : QWidget(parent)
{
    view_ = new VideoView;

    transport_ = new TransportBar(TransportBar::Kind::Source);
    transport_->onStart = [this] { seekTo(0.0); };
    transport_->onPreviousMark = [this] { stepToKeyframe(-1); };
    transport_->onPreviousFrame = [this] { stepFrame(-1); };
    transport_->onPlayPause = [this] { togglePlayPause(); };
    transport_->onStop = [this] { stop(); };
    transport_->onNextFrame = [this] { stepFrame(+1); };
    transport_->onNextMark = [this] { stepToKeyframe(+1); };
    transport_->onEnd = [this] { seekToEnd(); };
    transport_->onMarkIn = [this] { markIn(); };
    transport_->onMarkOut = [this] { markOut(); };
    transport_->onGoToIn = [this] { goToIn(); };
    transport_->onGoToOut = [this] { goToOut(); };
    transport_->onAdd = [this] {
        if (onAddRequested)
            onAddRequested();
    };
    transport_->onSpeedChosen = [this](double speed) { setBaseSpeed(speed); };
    transport_->onLoopToggled = [this](bool loop) { setLoop(loop); };
    transport_->onVolumeChanged = [this](int percent) { setVolume(percent); };
    transport_->onMuteToggled = [this](bool muted) { setMuted(muted); };

    timeline_ = new TimelineWidget;
    timeline_->onScrub = [this](double seconds) { updatePositionLabel(seconds); };
    timeline_->onSeek = [this](double seconds) { seekTo(seconds); };

    timeLabel_ = new QLabel;
    segmentLabel_ = new QLabel;
    // Le scritte non impongono una larghezza minima: in un pannello stretto si tagliano.
    timeLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    segmentLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    segmentLabel_->setTextFormat(Qt::RichText);
    // Sempre alta due righe (segmento + nota): così l'anteprima non cambia misura quando compare la nota.
    segmentLabel_->setMinimumHeight(fontMetrics().lineSpacing() * 2 + 4);
    decoderLabel_ = new QLabel;
    decoderLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // Prima riga: i pulsanti, con il tempo in mezzo; sotto il segmento IN-OUT e il decoder.
    transport_->setInfoWidget(timeLabel_);
    controls_ = new QWidget;
    auto* controls = new QVBoxLayout(controls_);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(0);
    controls->addWidget(transport_);
    auto* details = new QHBoxLayout;
    details->addWidget(segmentLabel_, 5);
    details->addWidget(decoderLabel_, 2);
    controls->addLayout(details);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // La misura minima la decide minimumSizeHint (piccola), non la somma delle righe: quelle che non ci stanno si
    // nascondono (updateCompact).
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->addWidget(view_, 1);
    layout->addWidget(timeline_);
    layout->addWidget(controls_);

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
    view_->onDoubleClicked = [this] {
        if (onVideoDoubleClicked)
            onVideoDoubleClicked();
    };
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
    return video_ != nullptr || ownAudio_;
}

void PlayerWidget::openFile(const QString& path)
{
    closeFile();
    const std::string utf8Path = QDir::toNativeSeparators(path).toStdString();

    QString videoError;
    bool noVideoStream = false;  // un file solo audio: niente da segnalare
    try {
        video_ = std::make_unique<core::VideoDecoder>(utf8Path);
    } catch (const core::NoStreamError& e) {
        videoError = QString::fromUtf8(e.what());
        noVideoStream = true;
    } catch (const std::exception& e) {
        videoError = QString::fromUtf8(e.what());
    }
    path_ = path;
    std::unique_ptr<core::AudioDecoder> own;
    try {
        own = std::make_unique<core::AudioDecoder>(utf8Path);
        ownAudioDuration_ = std::max(0.0, own->durationSeconds());
        ownAudio_ = true;
    } catch (const std::exception&) {
        // File senza audio: si riproduce solo il video.
    }

    if (!hasMedia()) {
        path_.clear();
        view_->setMessage(QStringLiteral("Anteprima non disponibile.\n%1").arg(videoError));
        updateControls();
        return;
    }
    openAudio(std::move(own));  // nessun thread è ancora partito: il decoder passa al thread dell'audio solo con play()

    duration_ = std::max({0.0, video_ ? video_->durationSeconds() : 0.0, ownAudioDuration_});
    timeline_->setDuration(duration_);
    position_ = 0.0;
    displayedTime_ = 0.0;
    frameDuration_ = video_ && video_->frameRate() > 0 ? 1.0 / video_->frameRate() : 0.04;
    timeline_->setFrameDuration(frameDuration_);
    // Per un video i fotogrammi chiave arrivano dall'analisi del file (setKeyframes); senza video non servono.
    segment_.reset({}, duration_);
    ranges_.clear();
    updateSegmentUi();

    if (video_) {
        const QSize pixels = view_->pixelSize();
        video_->setOutputSize(pixels.width(), pixels.height());
        showFrameAt(0.0);
    } else if (!videoError.isEmpty() && !noVideoStream)
        view_->setMessage(QStringLiteral("Video non riproducibile, solo audio.\n%1").arg(videoError));
    else
        view_->setMessage(QStringLiteral("Solo audio"));

    updateControls();
    updatePositionLabel(0.0);
}

void PlayerWidget::openAudio(std::unique_ptr<core::AudioDecoder> own)
{
    audio_.reset();
    audioOut_.close();
    externalActive_ = false;
    audioNote_.clear();
    if (path_.isEmpty() || (externalAudio_.isEmpty() && !ownAudio_))
        return;
    const QString source = externalAudio_.isEmpty() ? path_ : externalAudio_;
    try {
        if (externalAudio_.isEmpty() && own)
            audio_ = std::move(own);
        else
            audio_ = std::make_unique<core::AudioDecoder>(QDir::toNativeSeparators(source).toStdString());
        externalActive_ = !externalAudio_.isEmpty();
    } catch (const std::exception&) {
        audioNote_ = QStringLiteral("audio non leggibile");
        return;
    }
    // EDITORVIDEO_NO_AUDIO=1 simula un PC senza scheda audio (per provare quel caso anche dove la scheda c'è).
    const bool audioDisabled = qEnvironmentVariableIntValue("EDITORVIDEO_NO_AUDIO") != 0;
    if (audioDisabled || !audioOut_.open(audio_->sampleRate(), audio_->channels()))
        audioNote_ = QStringLiteral("audio non disponibile");
}

void PlayerWidget::setAudioMutes(std::vector<core::TimeRange> ranges)
{
    audioMutes_ = std::move(ranges);
}

void PlayerWidget::setExternalAudio(const QString& path, double delta)
{
    externalDelta_ = delta;
    if (path == externalAudio_)
        return;
    externalAudio_ = path;
    if (path_.isEmpty())
        return;
    const bool wasPlaying = playing_;
    if (wasPlaying)
        pause();
    openAudio();
    updateControls();
    if (wasPlaying)
        play();
}

void PlayerWidget::setBlank(bool blank)
{
    view_->setBlank(blank);
}

void PlayerWidget::closeFile()
{
    stopWorkers();
    playing_ = false;
    video_.reset();
    audio_.reset();
    audioOut_.close();
    path_.clear();
    ownAudio_ = false;
    ownAudioDuration_ = 0.0;
    decoderText_.clear();
    duration_ = 0.0;
    position_ = 0.0;
    displayedTime_ = 0.0;
    timeline_->setDuration(0.0);
    timeline_->setKeyframes({});
    segment_.reset({}, 0.0);
    ranges_.clear();
    segmentNote_.clear();
    updateSegmentUi();
    view_->setMessage(idleMessage_);
    updateControls();
    updatePositionLabel(0.0);
}

void PlayerWidget::togglePlayPause()
{
    if (playing_) {
        pause();
        speed_ = baseSpeed_;  // finisce la spola
        updateControls();
    } else {
        play();
    }
}

void PlayerWidget::play()
{
    if (!hasMedia() || playing_)
        return;
    if (speed_ < 0) {
        // Con una finestra (montaggio) ci pensa tick(): all'inizio del pezzo avvisa subito onWindowEnd.
        if (position_ <= 0.0005 && windowEnd_ <= 0) {
            speed_ = baseSpeed_;  // all'indietro dall'inizio: non c'è niente da riprodurre
            updateControls();
            return;
        }
    } else if (duration_ > 0 && position_ >= duration_ - 0.05) {
        position_ = 0.0;  // alla fine: si ricomincia dall'inizio
    }
    playStart_ = position_;
    hasPlayStart_ = true;
    startWorkers(position_);
    playing_ = true;
    timeline_->resumeFollowing();  // la linea del tempo torna a seguire il cursore
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

void PlayerWidget::stop()
{
    if (!hasMedia())
        return;
    if (playing_)
        pause();
    speed_ = baseSpeed_;
    if (hasPlayStart_)
        seekTo(playStart_);
    updateControls();
}

void PlayerWidget::setBaseSpeed(double speed)
{
    baseSpeed_ = speed > 0 ? speed : 1.0;
    setSpeed(baseSpeed_);
}

void PlayerWidget::setSpeed(double speed)
{
    if (speed == 0.0 || speed == speed_)
        return;
    speed_ = speed;
    if (playing_)
        restartAt(clock());
    updateControls();
}

void PlayerWidget::shuttle(int direction)
{
    if (!hasMedia())
        return;
    setSpeed(core::shuttleSpeed(speed_, playing_, direction));
    if (!playing_)
        play();
}

void PlayerWidget::setLoop(bool loop)
{
    loop_ = loop;
    updateControls();
}

void PlayerWidget::setVolume(int percent)
{
    volume_ = std::clamp(percent, 0, 100);
    if (volume_ > 0)
        muted_ = false;
    updateGain();
}

void PlayerWidget::setMuted(bool muted)
{
    muted_ = muted;
    updateGain();
}

void PlayerWidget::updateGain()
{
    // Al quadrato: il cursore segue meglio come si sente il volume.
    const float level = static_cast<float>(volume_) / 100.0f;
    gain_ = muted_ ? 0.0f : level * level;
    updateControls();
}

void PlayerWidget::restartAt(double position)
{
    stopWorkers();
    position_ = duration_ > 0 ? std::clamp(position, 0.0, duration_) : std::max(0.0, position);
    startWorkers(position_);
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

    workerSpeed_ = speed_;
    audioActive_ = audio_ != nullptr && audioOut_.isOpen() && core::audibleAtSpeed(speed_);
    audioEnded_ = !audioActive_;
    audioDelta_ = externalActive_ ? externalDelta_ : 0.0;
    audioFrom_ = from;
    workerMutes_ = audioMutes_;
    if (audioActive_) {
        audio_->seek(from + audioDelta_);
        audioOut_.reset();
        audioStart_ = from;
    }
    audioClock_ = audioActive_;
    wallBase_ = from;
    wallStart_ = std::chrono::steady_clock::now();

    stopWorkers_ = false;
    decoderAtDisplayed_ = false;  // il thread video porta avanti il decoder
    if (video_ && workerSpeed_ < 0)
        videoThread_ = std::thread([this, from] { videoReverseWorker(from); });
    else if (video_)
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

void PlayerWidget::videoReverseWorker(double from)
{
    // Il fotogramma `from` è già visibile: si parte da quello prima. Ogni blocco [inizio, fine) si decodifica in avanti
    // dal fotogramma chiave (seek), poi va nella coda dal più recente al più vecchio.
    double blockEnd = from - frameDuration_ / 2;
    try {
        while (!stopWorkers_ && blockEnd > 0.0) {
            const double length = std::min(kReverseBlockSeconds, kReverseBlockFrames * frameDuration_);
            const double blockStart = std::max(0.0, blockEnd - length);
            std::vector<std::shared_ptr<core::VideoFrame>> block;
            for (auto frame = video_->seek(blockStart); frame && frame->time < blockEnd && !stopWorkers_;
                 frame = video_->nextFrame()) {
                if (frame->time >= blockStart - frameDuration_ / 2)
                    block.push_back(std::move(frame));
            }
            if (stopWorkers_)
                break;
            // Il blocco dopo finisce dove inizia questo (prima di cedere i fotogrammi alla coda).
            const double earliest = block.empty() ? blockStart : std::min(blockStart, block.front()->time);
            for (auto it = block.rbegin(); it != block.rend(); ++it) {
                std::unique_lock lock(queueMutex_);
                queueSpace_.wait(lock, [this] { return stopWorkers_ || queue_.size() < kMaxQueuedFrames; });
                if (stopWorkers_)
                    break;
                queue_.push_back(std::move(*it));
            }
            blockEnd = earliest;
            if (blockStart <= 0.0)
                break;
        }
    } catch (const std::exception&) {
        // Errore di decodifica: ci si ferma sull'ultimo fotogramma mostrato.
    }
    std::lock_guard lock(queueMutex_);
    videoEnded_ = true;
}

void PlayerWidget::audioWorker()
{
    try {
        bool first = true;
        const int channels = audio_->channels();
        const double rate = audio_->sampleRate();
        const double speed = workerSpeed_;
        // A velocità diverse da 1x i campioni passano dal TimeStretch (stessa intonazione).
        std::optional<core::TimeStretch> stretch;
        if (speed != 1.0)
            stretch.emplace(channels, static_cast<int>(rate), speed);
        std::vector<std::int16_t> out;
        const auto send = [&](std::vector<std::int16_t>& samples) {
            const float gain = gain_;
            if (gain != 1.0f) {
                for (std::int16_t& v : samples)
                    v = static_cast<std::int16_t>(std::clamp(std::lround(v * gain), -32768L, 32767L));
            }
            return samples.empty() || audioOut_.write(samples.data(), samples.size(), stopWorkers_);
        };
        while (!stopWorkers_) {
            auto chunk = audio_->next();
            if (!chunk)
                break;
            const double time = chunk->time - audioDelta_;  // nei tempi del file
            if (first) {
                first = false;
                audioStart_ = time;
                // L'audio esterno può cominciare più avanti: fino a lì silenzio, così l'orologio parte dal punto giusto.
                const double gap = std::round((time - audioFrom_) * rate / speed);
                if (gap > 0 && gap < rate * 3600) {
                    audioStart_ = audioFrom_;
                    const bool written = core::writeSilence(
                        static_cast<std::size_t>(gap), channels, static_cast<int>(rate),
                        [this](const std::int16_t* samples, std::size_t count) {
                            return audioOut_.write(samples, count, stopWorkers_);
                        });
                    if (!written)
                        break;
                }
            }
            // Tratti senza audio: silenzio al posto dei campioni.
            const auto frames = static_cast<double>(chunk->samples.size() / channels);
            for (const core::TimeRange& m : workerMutes_) {
                const double from = std::clamp(std::ceil((m.start - time) * rate), 0.0, frames);
                const double to = std::clamp(std::ceil((m.end - time) * rate), 0.0, frames);
                if (to > from)
                    std::fill(chunk->samples.begin() + static_cast<std::ptrdiff_t>(from) * channels,
                              chunk->samples.begin() + static_cast<std::ptrdiff_t>(to) * channels, std::int16_t{0});
            }
            if (stretch) {
                out.clear();
                stretch->push(chunk->samples.data(), chunk->samples.size());
                stretch->pull(out);
                if (!send(out))
                    break;
            } else if (!send(chunk->samples)) {
                break;
            }
        }
        if (!stopWorkers_) {
            if (stretch) {
                out.clear();
                stretch->flush(out);
                send(out);
            }
            audioOut_.flush();
        }
    } catch (const std::exception&) {
        // Errore di decodifica: l'orologio passa al tempo reale e il video prosegue.
    }
    audioEnded_ = true;
}

double PlayerWidget::clock()
{
    if (audioClock_) {
        const double now =
            audioStart_.load() + audioOut_.playedFrames() / double(audioOut_.sampleRate()) * workerSpeed_;
        if (audioEnded_ && audioOut_.drained()) {
            // Audio finito (o più corto del video): da qui in poi si va avanti con il tempo reale.
            audioClock_ = false;
            wallBase_ = now;
            wallStart_ = std::chrono::steady_clock::now();
        }
        return now;
    }
    return wallBase_
         + workerSpeed_ * std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart_).count();
}

void PlayerWidget::tick()
{
    const double now = clock();
    const bool reverse = workerSpeed_ < 0;

    std::shared_ptr<core::VideoFrame> latest;
    bool videoDone = false;
    {
        std::lock_guard lock(queueMutex_);
        // Mostriamo l'ultimo fotogramma il cui tempo è già arrivato; quelli in ritardo vengono saltati. All'indietro
        // la coda va dal più recente al più vecchio, e "arrivato" vuol dire non prima di adesso.
        while (!queue_.empty()) {
            const double t = queue_.front()->time;
            const bool due = reverse ? t >= now : t <= now;
            const bool inWindow = windowEnd_ <= 0 || (reverse ? t >= windowStart_ - 0.001 : t < windowEnd_ - 0.001);
            if (!due || !inWindow)
                break;
            latest = std::move(queue_.front());
            queue_.pop_front();
        }
        videoDone = videoEnded_ && queue_.empty();
    }
    queueSpace_.notify_one();
    if (latest) {
        displayedTime_ = latest->time;
        view_->setFrame(std::move(latest));
    }

    position_ = duration_ > 0 ? std::clamp(now, 0.0, duration_) : std::max(0.0, now);
    // Il cursore segue ogni passo; il testo del tempo basta aggiornarlo ~30 volte al secondo (alla fine e in pausa
    // lo aggiorna comunque updatePositionLabel).
    timeline_->setPlayhead(position_);
    if (video_) {
        const auto [from, to] = video_->cachedRange();
        timeline_->setCacheRange(from, to);
    }
    if (!labelClock_.isValid() || labelClock_.elapsed() >= 33) {
        updatePositionLabel(position_);
        labelClock_.restart();
    }

    if (windowEnd_ > 0 && !reverse && now >= windowEnd_ - 0.001) {
        // Fine del pezzo: ci si ferma qui e lo si dice a chi riproduce la sequenza.
        stopAt(windowEnd_, true);
        return;
    }
    if (windowEnd_ > 0 && reverse && now <= windowStart_ + 0.001) {
        stopAt(windowStart_, true);  // all'indietro: inizio del pezzo
        return;
    }

    // Ripetizione fra IN e OUT (o tutto il file): arrivati in fondo si riparte dall'altro capo.
    if (loop_ && windowEnd_ <= 0) {
        const bool segment = segment_.hasIn() || segment_.hasOut();
        const double loopStart = segment ? segment_.in() : 0.0;
        const double loopEnd = segment ? segment_.out() : duration_;
        if (loopEnd > loopStart + frameDuration_) {
            if (!reverse && now >= loopEnd - 0.001) {
                restartAt(loopStart);
                return;
            }
            if (reverse && now <= loopStart + 0.001) {
                restartAt(loopEnd - frameDuration_);
                return;
            }
        }
    }

    if (reverse) {
        // All'indietro ci si ferma all'inizio del file.
        if (now <= 0.0005) {
            stopAt(0.0, false);
            speed_ = baseSpeed_;
            updateControls();
        }
        return;
    }

    core::PlaybackState state;
    state.hasVideo = video_ != nullptr;
    state.videoFinished = videoDone;
    state.audioActive = audioActive_;
    state.audioFinished = audioEnded_ && audioOut_.drained();
    state.clock = now;
    state.duration = duration_;
    if (core::playbackFinished(state)) {
        if (loop_ && windowEnd_ <= 0 && duration_ > 0) {
            restartAt(segment_.hasIn() ? segment_.in() : 0.0);  // ripetizione: il file finisce prima di OUT
            return;
        }
        // Un pezzo che arriva fino alla fine del file finisce qui, non al limite della finestra.
        stopAt(duration_ > 0 ? duration_ : position_, windowEnd_ > 0);
    }
}

void PlayerWidget::stopAt(double position, bool signalWindowEnd)
{
    stopWorkers();
    playing_ = false;
    position_ = position;
    updateControls();
    updatePositionLabel(position_);
    // onWindowEnd si chiama dopo, fuori da tick().
    if (signalWindowEnd) {
        QTimer::singleShot(0, this, [this] {
            if (onWindowEnd)
                onWindowEnd();
        });
    }
}

void PlayerWidget::showDecodeError(const std::exception& e)
{
    view_->setMessage(QStringLiteral("Errore di decodifica del video.\n%1").arg(QString::fromUtf8(e.what())));
}

void PlayerWidget::showFrameAt(double seconds)
{
    try {
        if (auto frame = video_->seek(seconds)) {
            displayedTime_ = frame->time;
            view_->setFrame(std::move(frame));
        }
        decoderAtDisplayed_ = true;
    } catch (const std::exception& e) {
        showDecodeError(e);
    }
    // Qui i thread sono fermi: si può leggere in sicurezza quale decoder è in uso.
    decoderText_ = QStringLiteral("Decodifica: %1").arg(QString::fromStdString(video_->decoderDescription()));
    updateControls();
}

void PlayerWidget::updateControls()
{
    const bool media = hasMedia();
    transport_->setMediaEnabled(media);
    timeline_->setEnabled(media && duration_ > 0);
    transport_->setPlaying(playing_);
    transport_->setSpeed(speed_);
    transport_->setLoop(loop_);
    transport_->setVolume(volume_, muted_);

    QString decoder;
    if (video_)
        decoder = decoderText_;  // aggiornato da showFrameAt()
    else if (ownAudio_)
        decoder = QStringLiteral("Solo audio");
    if (!audioNote_.isEmpty() && media)
        decoder += QStringLiteral("  ·  %1").arg(audioNote_);
    decoderLabel_->setText(media ? decoder : QString());
}

void PlayerWidget::updatePositionLabel(double seconds)
{
    timeLabel_->setText(QStringLiteral("%1 / %2").arg(timeText(seconds), timeText(duration_)));
    timeline_->setPlayhead(seconds);
    const auto [from, to] = cachedRange();
    timeline_->setCacheRange(from, to);
}

std::pair<double, double> PlayerWidget::cachedRange() const
{
    return video_ ? video_->cachedRange() : std::pair{0.0, 0.0};
}

void PlayerWidget::setKeyframes(const std::vector<double>& keyframes)
{
    if (!hasMedia())
        return;
    // Arrivano a file già aperto: IN e OUT eventualmente messi nel frattempo restano (non si agganciano più).
    segment_.setKeyframes(keyframes);
    timeline_->setKeyframes(keyframes);
    updateSegmentUi();
}

double PlayerWidget::markTime() const
{
    // Con il video si segna il fotogramma che si vede, altrimenti la posizione corrente.
    return video_ ? displayedTime_ : position_;
}

void PlayerWidget::markIn()
{
    if (!hasMedia())
        return;
    if (playing_)
        pause();
    const double in = segment_.setIn(markTime());
    segmentNote_.clear();
    noteIsOutError_ = false;
    // Fra due fotogrammi chiave (se già noti): si può, la parte fino al fotogramma chiave dopo si ricodificherà.
    if (std::abs(segment_.snapToKeyframe(in) - in) > 0.0005)
        segmentNote_ = QStringLiteral("IN fra due fotogrammi chiave: all'esportazione la parte da qui al fotogramma chiave "
                                      "dopo viene ricodificata (taglio preciso), il resto si copia.");
    updateSegmentUi();
}

void PlayerWidget::markOut()
{
    if (!hasMedia())
        return;
    if (playing_)
        pause();
    // OUT comprende il fotogramma visibile: il segmento finisce alla fine di quel fotogramma.
    const double wanted = video_ ? displayedTime_ + frameDuration_ : position_;
    if (segment_.setOut(wanted)) {
        // La spiegazione su IN resta visibile; si toglie solo un eventuale errore su OUT.
        if (noteIsOutError_)
            segmentNote_.clear();
        noteIsOutError_ = false;
    } else {
        segmentNote_ = QStringLiteral("OUT deve stare dopo IN (%1).").arg(timeText(segment_.in()));
        noteIsOutError_ = true;
    }
    updateSegmentUi();
}

void PlayerWidget::clearSegment()
{
    segment_.clearIn();
    segment_.clearOut();
    segmentNote_.clear();
    updateSegmentUi();
}

void PlayerWidget::goToIn()
{
    seekTo(segment_.in());
}

void PlayerWidget::goToOut()
{
    // L'ultimo fotogramma compreso nel segmento.
    seekTo(std::max(segment_.in(), segment_.out() - (video_ ? frameDuration_ : 0.0)));
}

void PlayerWidget::stepFrame(int direction)
{
    if (!hasMedia())
        return;
    if (playing_)
        pause();
    if (!video_) {
        // Solo audio: niente fotogrammi, ci si sposta di 1/25 di secondo.
        seekTo(position_ + direction * frameDuration_);
        return;
    }
    try {
        if (direction > 0) {
            // Il decoder deve stare subito dopo il fotogramma visibile: dopo una riproduzione non è così.
            if (!decoderAtDisplayed_)
                showFrameAt(displayedTime_);
            if (auto frame = video_->nextFrame()) {
                displayedTime_ = frame->time;
                view_->setFrame(std::move(frame));
            }
        } else {
            showFrameAt(std::max(0.0, displayedTime_ - frameDuration_));
        }
    } catch (const std::exception& e) {
        showDecodeError(e);
    }
    position_ = displayedTime_;
    updatePositionLabel(position_);
}

void PlayerWidget::stepToKeyframe(int direction)
{
    if (!hasMedia())
        return;
    const std::vector<double>& keyframes = segment_.keyframes();
    if (keyframes.empty())
        return;
    if (playing_)
        pause();
    const double at = markTime();
    double target = at;
    if (direction > 0) {
        const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), at + 0.001);
        if (next == keyframes.end())
            return;
        target = *next;
    } else {
        const auto next = std::lower_bound(keyframes.begin(), keyframes.end(), at - 0.001);
        if (next == keyframes.begin())
            return;
        target = *std::prev(next);
    }
    seekTo(target);
}

void PlayerWidget::seekToEnd()
{
    if (!hasMedia())
        return;
    if (playing_)
        pause();
    seekTo(std::max(0.0, duration_ - (video_ ? frameDuration_ : 0.0)));
}

void PlayerWidget::updateSegmentUi()
{
    timeline_->setSegment(segment_.in(), segment_.out(), segment_.hasIn(), segment_.hasOut());
    timeline_->setKeptRanges(ranges_);
    if (onSegmentChanged)
        onSegmentChanged();
    if (!hasMedia()) {
        segmentLabel_->clear();
        return;
    }
    const auto marker = [](bool set, double seconds, const char* color) {
        return set ? QStringLiteral("<span style='color:%1'><b>%2</b></span>").arg(QLatin1String(color), timeText(seconds))
                   : QStringLiteral("<i>%1</i>").arg(timeText(seconds));
    };
    QString text = QStringLiteral("IN %1 &nbsp; OUT %2 &nbsp; Segmento <b>%3</b>")
                       .arg(marker(segment_.hasIn(), segment_.in(), "#46be5a"),
                            marker(segment_.hasOut(), segment_.out(), "#e15046"), timeText(segment_.length()));
    if (!ranges_.empty()) {
        double used = 0.0;
        for (const core::TimeRange& r : ranges_)
            used += r.length();
        text += QStringLiteral(" &nbsp;·&nbsp; Di questo file nel montaggio: <b>%1</b> tratti, <b>%2</b>")
                    .arg(ranges_.size())
                    .arg(timeText(used));
    }
    if (!segmentNote_.isEmpty())
        text += QStringLiteral("<br><small>%1</small>").arg(segmentNote_.toHtmlEscaped());
    segmentLabel_->setText(text);
}

const core::CutSegment& PlayerWidget::segment() const
{
    return segment_;
}

void PlayerWidget::setPlaybackWindow(double start, double end)
{
    windowStart_ = start;
    windowEnd_ = end;
}

void PlayerWidget::setIdleMessage(const QString& message)
{
    idleMessage_ = message;
    if (!hasMedia())
        view_->setMessage(idleMessage_);
}

void PlayerWidget::setChromeVisible(bool visible)
{
    chromeVisible_ = visible;
    timeline_->setVisible(visible);
    controls_->setVisible(visible);
    updateCompact();
}

QSize PlayerWidget::minimumSizeHint() const
{
    return {120, chromeVisible_ ? 64 : 36};
}

void PlayerWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateCompact();
}

void PlayerWidget::updateCompact()
{
    if (!chromeVisible_)
        return;
    // Dal meno importante: decoder e segmento (righe di testo), poi la linea del tempo.
    const int h = height();
    segmentLabel_->setVisible(h >= 220);
    decoderLabel_->setVisible(h >= 220 && width() >= 420);
    timeline_->setVisible(h >= 140);
}

void PlayerWidget::setInOut(double in, double out)
{
    segment_.setIn(in);
    segment_.setOut(out);
    segmentNote_.clear();
    noteIsOutError_ = false;
    updateSegmentUi();
}

void PlayerWidget::showNote(const QString& note)
{
    segmentNote_ = note;
    noteIsOutError_ = false;
    updateSegmentUi();
}

void PlayerWidget::setRanges(std::vector<core::TimeRange> ranges)
{
    ranges_ = std::move(ranges);
    segmentNote_.clear();
    noteIsOutError_ = false;
    updateSegmentUi();
}

}  // namespace editor::app
