#include "EditorApp/ThumbnailCache.h"

#include "EditorCore/Thumbnails.h"

#include <QtCore/QTimer>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace editor::app {

ThumbnailCache::ThumbnailCache(QObject* owner, std::function<void()> onChanged)
    : owner_(owner)
    , onChanged_(std::move(onChanged))
{
}

ThumbnailCache::~ThumbnailCache()
{
    if (thread_ != nullptr) {
        cancel_ = true;
        thread_->wait();
    }
}

QImage ThumbnailCache::get(const std::string& path, double seconds)
{
    const QString key = QStringLiteral("%1|%2").arg(QString::fromStdString(path)).arg(std::llround(seconds * 1000));
    if (const QImage* ready = images_.object(key))
        return *ready;
    if (!pending_.contains(key)) {
        pending_.insert(key, true);
        queue_.push_back({key, path, seconds});
        // Si parte al prossimo giro del ciclo degli eventi, non durante il disegno della timeline.
        QTimer::singleShot(0, owner_, [this] { startNext(); });
    }
    return {};
}

int ThumbnailCache::ready() const
{
    int count = 0;
    for (const QString& key : images_.keys())
        count += images_.object(key)->isNull() ? 0 : 1;
    return count;
}

void ThumbnailCache::startNext()
{
    if (thread_ != nullptr || queue_.empty())
        return;
    // Tutte le richieste in attesa in un colpo, raggruppate per file: un decoder aperto per file.
    auto batch = std::make_shared<std::vector<Request>>(queue_.begin(), queue_.end());
    queue_.clear();
    std::stable_sort(batch->begin(), batch->end(), [](const Request& a, const Request& b) { return a.path < b.path; });
    auto results = std::make_shared<std::vector<QImage>>(batch->size());
    cancel_ = false;
    thread_ = QThread::create([batch, results, cancel = &cancel_] {
        core::ThumbnailExtractor extractor(192, 108, true);  // con la GPU, se c'è
        for (std::size_t i = 0; i < batch->size() && !cancel->load(); ++i) {
            const auto frame = extractor.extract((*batch)[i].path, (*batch)[i].seconds);
            if (frame)
                (*results)[i] = QImage(frame->pixels.data(), frame->width, frame->height, frame->stride, QImage::Format_RGB32).copy();
        }
    });
    thread_->setParent(owner_);
    QObject::connect(thread_, &QThread::finished, owner_, [this, batch, results] {
        thread_->deleteLater();
        thread_ = nullptr;
        for (std::size_t i = 0; i < batch->size(); ++i) {
            if (!cancel_) {
                // Costo in kilobyte: l'immagine vuota (il file non ha video) costa 1.
                const QImage& image = (*results)[i];
                images_.insert((*batch)[i].key, new QImage(image), static_cast<int>(image.sizeInBytes() / 1024) + 1);
            }
            pending_.remove((*batch)[i].key);
        }
        onChanged_();
        startNext();
    });
    // Bassa priorità: l'anteprima e l'analisi dei file vengono prima.
    thread_->start(QThread::LowPriority);
}

}  // namespace editor::app
