#include "EditorApp/WaveformStore.h"

#include "EditorCore/Waveform.h"

#include <algorithm>

namespace editor::app {

WaveformStore::WaveformStore(QObject* owner, std::function<void()> onChanged)
    : owner_(owner)
    , onChanged_(std::move(onChanged))
{
}

WaveformStore::~WaveformStore()
{
    // Se un calcolo è ancora in corso lo interrompiamo e aspettiamo che il thread finisca.
    if (thread_ != nullptr) {
        cancel_ = true;
        thread_->wait();
    }
}

std::shared_ptr<const core::Waveform> WaveformStore::get(const std::string& path) const
{
    const auto it = waveforms_.find(path);
    return it != waveforms_.end() ? it->second : nullptr;
}

int WaveformStore::pending() const
{
    return static_cast<int>(queue_.size()) + (thread_ != nullptr ? 1 : 0);
}

void WaveformStore::request(const std::string& path)
{
    if (waveforms_.contains(path) || path == computing_ || std::find(queue_.begin(), queue_.end(), path) != queue_.end())
        return;
    queue_.push_back(path);
    startNext();
}

void WaveformStore::startNext()
{
    if (thread_ != nullptr || queue_.empty())
        return;
    computing_ = queue_.front();
    queue_.pop_front();
    auto result = std::make_shared<core::Waveform>();
    cancel_ = false;
    thread_ = QThread::create([path = computing_, result, cancel = &cancel_] { *result = core::computeWaveform(path, cancel); });
    thread_->setParent(owner_);
    QObject::connect(thread_, &QThread::finished, owner_, [this, result] {
        thread_->deleteLater();
        thread_ = nullptr;
        if (!cancel_)
            waveforms_[computing_] = result;
        computing_.clear();
        onChanged_();
        startNext();
    });
    // Bassa priorità: l'anteprima e l'analisi dei file vengono prima.
    thread_->start(QThread::LowPriority);
}

}  // namespace editor::app
