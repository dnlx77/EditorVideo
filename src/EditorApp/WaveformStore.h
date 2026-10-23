#pragma once

#include <QtCore/QThread>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace editor::core {
struct Waveform;
}

namespace editor::app {

// Le forme d'onda dei file (per percorso): si chiedono con request(), si calcolano una alla volta in background e si
// tengono da parte. Ogni volta che ne finisce una si chiama `onChanged` (nel thread della finestra).
class WaveformStore
{
public:
    // `owner` è l'oggetto che riceve la fine del calcolo (e che possiede il thread).
    WaveformStore(QObject* owner, std::function<void()> onChanged);
    ~WaveformStore();
    WaveformStore(const WaveformStore&) = delete;
    WaveformStore& operator=(const WaveformStore&) = delete;

    std::shared_ptr<const core::Waveform> get(const std::string& path) const;  // nullptr = non ancora pronta
    void request(const std::string& path);
    int pending() const;  // in calcolo o in coda
    // Toglie le richieste in coda; quelle già calcolate restano (servono se si riapre lo stesso file).
    void clearQueue() { queue_.clear(); }

private:
    void startNext();

    QObject* owner_;
    std::function<void()> onChanged_;
    std::map<std::string, std::shared_ptr<const core::Waveform>> waveforms_;
    std::deque<std::string> queue_;
    std::string computing_;  // in calcolo
    QThread* thread_ = nullptr;
    std::atomic<bool> cancel_{false};
};

}  // namespace editor::app
