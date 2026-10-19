#pragma once

#include <QtWidgets/QMainWindow>

#include <atomic>
#include <chrono>

class QAction;
class QLabel;
class QStackedWidget;
class QThread;
class QTreeWidget;

namespace editor::core {
struct MediaInfo;
}

namespace editor::app {

class PlayerWidget;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Apre l'anteprima e analizza il file in un thread separato per mostrarne le informazioni.
    void openPath(const QString& path);

    PlayerWidget* player() const { return player_; }
    QString currentFile() const { return currentPath_; }

    // Esporta il segmento IN-OUT del file aperto (chiede dove salvarlo).
    void exportSegment();
    // Esportazione verso `target` senza chiedere niente; showResult = false per non mostrare il messaggio finale
    // (prove automatiche). false se non si può partire (IN/OUT mancanti o esportazione già in corso).
    // slowDown rallenta di proposito ogni passo (solo per le prove: così la finestra di avanzamento compare).
    bool startExport(const QString& target, bool showResult, std::chrono::milliseconds slowDown = {});
    bool isExporting() const { return exportThread_ != nullptr; }
    bool lastExportSucceeded() const { return lastExportOk_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createMenus();
    void openFile();
    void showAbout();
    void showInfo(const core::MediaInfo& info);
    void setBusy(bool busy);
    void updateExportAction();

    QStackedWidget* pages_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QTreeWidget* infoTree_ = nullptr;
    QLabel* keyframeTitle_ = nullptr;
    QTreeWidget* keyframeList_ = nullptr;
    PlayerWidget* player_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QString currentPath_;  // file aperto (percorso nativo)

    QThread* probeThread_ = nullptr;
    std::atomic<bool> cancelProbe_{false};
    QThread* exportThread_ = nullptr;
    std::atomic<bool> cancelExport_{false};
    bool lastExportOk_ = false;
};

// Testo con le versioni di Qt e FFmpeg, usato nella barra di stato e in "Informazioni".
QString versionSummary();

}  // namespace editor::app
