#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QThread>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <set>

namespace editor::app {

// I proxy dei file (vedi core::buildProxy) per la qualità dell'anteprima scelta: si chiedono con request(), si creano
// uno alla volta in background (con la scheda video) nella cartella dei proxy, e videoSource() dice da dove leggere il
// video: il proxy se è pronto, altrimenti l'originale. Le funzioni on... si chiamano nel thread della finestra.
class ProxyManager
{
public:
    explicit ProxyManager(QObject* owner);
    ~ProxyManager();
    ProxyManager(const ProxyManager&) = delete;
    ProxyManager& operator=(const ProxyManager&) = delete;

    // Cartella dei proxy (accanto al progetto, o quella provvisoria per un progetto mai salvato).
    void setFolder(const QString& folder);
    QString folder() const { return folder_; }
    // 1 = originali (nessun proxy), 2, 4, 8 = frazione della risoluzione. Cambiandola la coda si svuota.
    void setDivisor(int divisor);
    int divisor() const { return divisor_; }

    // Il file da cui leggere il video di `original`: il suo proxy se è pronto, altrimenti `original`.
    QString videoSource(const QString& original) const;
    bool hasProxy(const QString& original) const { return videoSource(original) != original; }
    // Crea i proxy mancanti di questi file, nell'ordine (il primo per primo: di solito quello che si sta guardando).
    void request(const QStringList& originals);
    // Avanzamento (0-1) del proxy di `original` se è in coda o in lavoro; -1 altrimenti.
    double progress(const QString& original) const;
    // Sposta nella cartella `folder` i proxy già pronti di questi file e la usa da qui in poi (es. progetto salvato).
    void moveTo(const QString& folder, const QStringList& originals);
    // Riprova a cancellare i proxy che moveTo ha copiato ma non ha potuto togliere (erano aperti in un'anteprima):
    // da chiamare dopo aver riaperto le anteprime dalla nuova cartella.
    void removeLeftovers();
    // Elimina i proxy (di tutte le qualità) di questi file dalla cartella; ferma quello in lavoro.
    void removeAll(const QStringList& originals);

    std::function<void(const QString& original)> onReady;
    std::function<void(const QString& original)> onProgress;  // l'avanzamento è cambiato di almeno un punto
    std::function<void(const QString& original, const QString& error)> onFailed;

private:
    QString proxyPath(const QString& original, int divisor) const;  // vuoto se l'originale non c'è
    void startNext();
    void cancelCurrent();

    QObject* owner_;
    QString folder_;
    int divisor_ = 1;
    std::deque<QString> queue_;
    QString working_;  // in lavoro
    std::shared_ptr<std::atomic<double>> workingProgress_;
    QThread* thread_ = nullptr;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::set<QString> failed_;  // "divisore|percorso": non si riprova nella stessa sessione
    QStringList leftovers_;     // vedi removeLeftovers
};

}  // namespace editor::app
