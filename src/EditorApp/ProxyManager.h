#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QThread>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace editor::app {

// I proxy dei file (vedi core::buildProxy). Ogni file ha la sua qualità dell'anteprima (setDivisor): i proxy si
// chiedono con request(), si creano uno alla volta in background (con la scheda video) nella cartella dei proxy, e
// videoSource() dice da dove leggere il video: il proxy se è pronto, altrimenti l'originale. Le funzioni on... si
// chiamano nel thread della finestra.
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
    // Qualità dell'anteprima di un file: 1 = l'originale (nessun proxy), 2, 4, 8 = frazione della risoluzione. Cambiandola
    // si interrompe solo il proxy di quel file (se era in creazione); gli altri vanno avanti.
    void setDivisor(const QString& original, int divisor);
    int divisor(const QString& original) const;
    const std::map<QString, int>& divisors() const { return divisors_; }  // solo i file con un proxy (2, 4, 8)
    void clearDivisors();  // tutti i file tornano all'originale (es. nuovo progetto)

    // Il file da cui leggere il video di `original`: il suo proxy della qualità scelta se è pronto, altrimenti un suo
    // proxy di un'altra qualità già pronto (es. 1/2 mentre si crea quello a 1/4), altrimenti `original`.
    QString videoSource(const QString& original) const;
    // La qualità del file che dà videoSource: 1 = l'originale, 2, 4, 8 = un proxy.
    int sourceDivisor(const QString& original) const;
    // true se il proxy della qualità scelta per il file è pronto.
    bool hasProxy(const QString& original) const
    {
        return divisor(original) > 1 && !readyProxy(original, divisor(original)).isEmpty();
    }
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

    std::function<void(const QString& original, const QString& how)> onReady;  // how: come è stato creato
    std::function<void(const QString& original)> onProgress;  // l'avanzamento è cambiato di almeno un punto
    std::function<void(const QString& original, const QString& error)> onFailed;

private:
    QString proxyPath(const QString& original, int divisor) const;  // vuoto se l'originale non c'è
    QString readyProxy(const QString& original, int divisor) const;  // il proxy se esiste, altrimenti vuoto
    void startNext();
    void cancelCurrent();

    QObject* owner_;
    QString folder_;
    std::map<QString, int> divisors_;  // qualità scelta per file (manca = l'originale)
    std::deque<QString> queue_;
    QString working_;  // in lavoro
    int workingDivisor_ = 0;
    std::shared_ptr<std::atomic<double>> workingProgress_;
    QThread* thread_ = nullptr;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::set<QString> failed_;  // "divisore|percorso": non si riprova nella stessa sessione
    QStringList leftovers_;     // vedi removeLeftovers
};

}  // namespace editor::app
