#include "EditorApp/ProxyManager.h"

#include "EditorCore/Proxy.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>

#include <algorithm>
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

void ProxyManager::setDivisor(int divisor)
{
    if (divisor == divisor_)
        return;
    queue_.clear();
    cancelCurrent();
    divisor_ = divisor;
}

QString ProxyManager::proxyPath(const QString& original, int divisor) const
{
    if (folder_.isEmpty())
        return {};
    const std::string name = core::proxyFileName(QDir::toNativeSeparators(original).toStdString(), divisor);
    return name.empty() ? QString() : QDir::toNativeSeparators(QDir(folder_).filePath(QString::fromStdString(name)));
}

QString ProxyManager::videoSource(const QString& original) const
{
    if (divisor_ <= 1)
        return original;
    const QString proxy = proxyPath(original, divisor_);
    return !proxy.isEmpty() && QFileInfo::exists(proxy) ? proxy : original;
}

double ProxyManager::progress(const QString& original) const
{
    if (divisor_ <= 1)
        return -1.0;
    if (original == working_ && workingProgress_)
        return workingProgress_->load();
    return std::find(queue_.begin(), queue_.end(), original) != queue_.end() ? 0.0 : -1.0;
}

void ProxyManager::request(const QStringList& originals)
{
    if (divisor_ <= 1 || folder_.isEmpty())
        return;
    // Quelli chiesti ora passano davanti, nel loro ordine.
    std::deque<QString> wanted;
    for (const QString& original : originals) {
        if (original == working_ || hasProxy(original) || failed_.contains(failureKey(divisor_, original))
            || std::find(wanted.begin(), wanted.end(), original) != wanted.end() || proxyPath(original, divisor_).isEmpty())
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
    if (thread_ != nullptr || queue_.empty())
        return;
    working_ = queue_.front();
    queue_.pop_front();
    const QString target = proxyPath(working_, divisor_);
    const int divisor = divisor_;
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto error = std::make_shared<QString>();
    workingProgress_ = progress;
    cancel_ = cancel;
    QObject* owner = owner_;
    thread_ = QThread::create([this, owner, original = working_, target, divisor, progress, cancel, error] {
        int lastPercent = -1;
        try {
            core::buildProxy(
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
        } catch (const std::exception& e) {
            *error = QString::fromUtf8(e.what());
        }
    });
    thread_->setParent(owner_);
    QObject::connect(thread_, &QThread::finished, owner_, [this, cancel, error, divisor] {
        thread_->deleteLater();
        thread_ = nullptr;
        const QString original = working_;
        working_.clear();
        workingProgress_.reset();
        if (!cancel->load()) {
            if (error->isEmpty()) {
                if (onReady)
                    onReady(original);
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
