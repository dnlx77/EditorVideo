#pragma once

#include <QtCore/QCache>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtGui/QImage>

#include <atomic>
#include <deque>
#include <functional>
#include <string>

namespace editor::app {

// Le miniature della linea del tempo, per "percorso|millisecondi" (immagine vuota = il file non ha video). In memoria
// al massimo 64 MB: le più vecchie escono e, se servono di nuovo, si rifanno. Le richieste si raggruppano e si
// estraggono in background; quando un gruppo è pronto si chiama `onChanged` (nel thread della finestra).
class ThumbnailCache
{
public:
    ThumbnailCache(QObject* owner, std::function<void()> onChanged);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // La miniatura del fotogramma di `path` a `seconds` (già portato a un fotogramma chiave dal chiamante); se non c'è
    // ancora, un'immagine vuota e la richiesta parte.
    QImage get(const std::string& path, double seconds);
    int pending() const { return static_cast<int>(pending_.size()); }
    int ready() const;

private:
    static constexpr int kCacheKilobytes = 64 * 1024;
    struct Request
    {
        QString key;
        std::string path;
        double seconds = 0.0;
    };

    void startNext();

    QObject* owner_;
    std::function<void()> onChanged_;
    QCache<QString, QImage> images_{kCacheKilobytes};
    std::deque<Request> queue_;
    QHash<QString, bool> pending_;
    QThread* thread_ = nullptr;
    std::atomic<bool> cancel_{false};
};

}  // namespace editor::app
