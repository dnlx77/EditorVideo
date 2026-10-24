#include "EditorApp/ProxyManager.h"

#include "EditorCore/Proxy.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>

#include <algorithm>
#include <cmath>
#include <exception>

namespace editor::app {

namespace {

QString failureKey(int divisor, const QString& original)
{
    return QString::number(divisor) + QLatin1Char('|') + original;
}

}  // namespace

ProxyManager::ProxyManager(QObject* owner)
    : owner_(owner)
{
}

ProxyManager::~ProxyManager()
{
    // Un proxy in lavoro si interrompe (il file a metà si cancella da solo) e si aspetta la fine del thread.
    if (thread_ != nullptr) {
        cancel_->store(true);
        thread_->wait();
    }
}

void ProxyManager::setFolder(const QString& folder)
{
    if (QDir::cleanPath(folder) == QDir::cleanPath(folder_))
        return;
    cancelCurrent();
    folder_ = folder;
}

int ProxyManager::divisor(const QString& original) const
{
    const auto found = divisors_.find(original);
    return found != divisors_.end() ? found->second : 1;
}

void ProxyManager::setDivisor(const QString& original, int divisor)
{
    if (divisor == this->divisor(original))
        return;
    if (divisor <= 1)
        divisors_.erase(original);
    else
        divisors_[original] = divisor;
    // Solo questo file: se si stava creando il suo proxy di un'altra qualità si interrompe; quelli degli altri file
    // vanno avanti.
    queue_.erase(std::remove(queue_.begin(), queue_.end(), original), queue_.end());
    if (thread_ != nullptr && working_ == original)
        cancelCurrent();
}

void ProxyManager::clearDivisors()
{
    divisors_.clear();
    queue_.clear();
    cancelCurrent();
}

QString ProxyManager::proxyPath(const QString& original, int divisor) const
{
    if (folder_.isEmpty())
        return {};
    const std::string name = core::proxyFileName(QDir::toNativeSeparators(original).toStdString(), divisor);
    return name.empty() ? QString() : QDir::toNativeSeparators(QDir(folder_).filePath(QString::fromStdString(name)));
}

QString ProxyManager::readyProxy(const QString& original, int divisor) const
{
    const QString proxy = proxyPath(original, divisor);
    return !proxy.isEmpty() && QFileInfo::exists(proxy) ? proxy : QString();
}

int ProxyManager::sourceDivisor(const QString& original) const
{
    const int chosen = divisor(original);
    if (chosen <= 1)
        return 1;
    if (!readyProxy(original, chosen).isEmpty())
        return chosen;
    // Quello della qualità scelta non è ancora pronto: meglio un proxy di un'altra qualità che l'originale. Il più
    // vicino alla qualità scelta, a pari distanza quello più definito.
    const auto distance = [&](int d) { return std::abs(std::log2(static_cast<double>(d) / chosen)); };
    int best = 1;
    for (const int divisor : core::kPreviewDivisors) {
        if (divisor <= 1 || divisor == chosen || readyProxy(original, divisor).isEmpty())
            continue;
        if (best == 1 || distance(divisor) < distance(best) - 1e-9)
            best = divisor;
    }
    return best;
}

QString ProxyManager::videoSource(const QString& original) const
{
    const int divisor = sourceDivisor(original);
    return divisor > 1 ? readyProxy(original, divisor) : original;
}

double ProxyManager::progress(const QString& original) const
{
    if (divisor(original) <= 1)
        return -1.0;
    if (original == working_ && workingDivisor_ == divisor(original) && workingProgress_)
        return workingProgress_->load();
    return std::find(queue_.begin(), queue_.end(), original) != queue_.end() ? 0.0 : -1.0;
}

void ProxyManager::request(const QStringList& originals)
{
    if (folder_.isEmpty())
        return;
    // Quelli chiesti ora passano davanti, nel loro ordine. Ognuno con la qualità scelta per lui.
    std::deque<QString> wanted;
    for (const QString& original : originals) {
        const int chosen = divisor(original);
        if (chosen <= 1 || (original == working_ && workingDivisor_ == chosen) || !readyProxy(original, chosen).isEmpty()
            || failed_.contains(failureKey(chosen, original)) || std::find(wanted.begin(), wanted.end(), original) != wanted.end()
            || proxyPath(original, chosen).isEmpty())
            continue;
        wanted.push_back(original);
    }
    for (const QString& queued : queue_) {
        if (std::find(wanted.begin(), wanted.end(), queued) == wanted.end())
            wanted.push_back(queued);
    }
    queue_ = std::move(wanted);
    startNext();
}

void ProxyManager::startNext()
{
    // Salta quelli tornati all'originale o già pronti nel frattempo.
    while (!queue_.empty() && (divisor(queue_.front()) <= 1 || !readyProxy(queue_.front(), divisor(queue_.front())).isEmpty()))
        queue_.pop_front();
    if (thread_ != nullptr || queue_.empty())
        return;
    working_ = queue_.front();
    queue_.pop_front();
    const int divisor = this->divisor(working_);
    workingDivisor_ = divisor;
    const QString target = proxyPath(working_, divisor);
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto error = std::make_shared<QString>();
    auto how = std::make_shared<QString>();
    workingProgress_ = progress;
    cancel_ = cancel;
    QObject* owner = owner_;
    thread_ = QThread::create([this, owner, original = working_, target, divisor, progress, cancel, error, how] {
        int lastPercent = -1;
        try {
            const core::ProxyInfo info = core::buildProxy(
                QDir::toNativeSeparators(original).toStdString(), target.toStdString(), divisor,
                [&](double p) {
                    progress->store(p);
                    const int percent = static_cast<int>(p * 100);
                    if (percent == lastPercent)
                        return;
                    lastPercent = percent;
                    QMetaObject::invokeMethod(owner, [this, original, cancel] {
                        if (!cancel->load() && onProgress)
                            onProgress(original);
                    }, Qt::QueuedConnection);
                },
                cancel.get());
            *how = QStringLiteral("%1, codifica %2")
                       .arg(info.gpuResize ? QStringLiteral("decodifica e riduzione con la GPU NVIDIA")
                                           : QStringLiteral("decodifica e riduzione con il processore"),
                            QString::fromStdString(info.encoder));
        } catch (const std::exception& e) {
            *error = QString::fromUtf8(e.what());
        }
    });
    thread_->setParent(owner_);
    QObject::connect(thread_, &QThread::finished, owner_, [this, cancel, error, how, divisor] {
        thread_->deleteLater();
        thread_ = nullptr;
        const QString original = working_;
        working_.clear();
        workingDivisor_ = 0;
        workingProgress_.reset();
        if (!cancel->load()) {
            if (error->isEmpty()) {
                if (onReady)
                    onReady(original, *how);
            } else {
                failed_.insert(failureKey(divisor, original));
                if (onFailed)
                    onFailed(original, *error);
            }
        }
        startNext();
    });
    // Bassa priorità: l'anteprima viene prima.
    thread_->start(QThread::LowPriority);
}

void ProxyManager::cancelCurrent()
{
    if (thread_ == nullptr)
        return;
    // Il thread si ferma presto (buildProxy controlla l'interruzione a ogni pacchetto) e cancella il file a metà; se il
    // proxy serve ancora lo si richiede e riparte da capo.
    cancel_->store(true);
    thread_->wait();
}

void ProxyManager::moveTo(const QString& folder, const QStringList& originals)
{
    if (QDir::cleanPath(folder) == QDir::cleanPath(folder_))
        return;
    const QString interrupted = thread_ != nullptr ? working_ : QString();
    cancelCurrent();
    const QString from = folder_;
    QDir().mkpath(folder);
    for (const QString& original : originals) {
        for (const int divisor : core::kPreviewDivisors) {
            if (divisor <= 1)
                continue;
            const QString source = proxyPath(original, divisor);
            if (source.isEmpty() || !QFileInfo::exists(source))
                continue;
            const QString target = QDir(folder).filePath(QFileInfo(source).fileName());
            QFile::remove(target);
            if (!QFile::rename(source, target)) {
                // Su un altro disco, o se il proxy è aperto in un'anteprima: copia, e il vecchio si cancella appena si
                // può (removeLeftovers).
                if (QFile::copy(source, target) && !QFile::remove(source))
                    leftovers_ << source;
            }
        }
    }
    folder_ = folder;
    QDir(from).rmdir(QStringLiteral("."));  // la vecchia cartella, se è rimasta vuota
    if (!interrupted.isEmpty())
        request({interrupted});
}

void ProxyManager::removeLeftovers()
{
    QStringList still;
    for (const QString& file : leftovers_) {
        if (QFileInfo::exists(file) && !QFile::remove(file))
            still << file;
        else
            QDir(QFileInfo(file).path()).rmdir(QStringLiteral("."));  // la cartella, se è rimasta vuota
    }
    leftovers_ = still;
}

void ProxyManager::removeAll(const QStringList& originals)
{
    queue_.clear();
    cancelCurrent();
    for (const QString& original : originals) {
        for (const int divisor : core::kPreviewDivisors) {
            const QString proxy = divisor > 1 ? proxyPath(original, divisor) : QString();
            if (!proxy.isEmpty())
                QFile::remove(proxy);
        }
    }
    failed_.clear();
}

}  // namespace editor::app
