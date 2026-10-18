#pragma once

#include <QtWidgets/QMainWindow>

#include <atomic>

class QAction;
class QLabel;
class QStackedWidget;
class QThread;
class QTreeWidget;

namespace editor::core {
struct MediaInfo;
}

namespace editor::app {

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Analizza il file in un thread separato e ne mostra le informazioni.
    void openPath(const QString& path);

private:
    void createMenus();
    void openFile();
    void showAbout();
    void showInfo(const core::MediaInfo& info);
    void setBusy(bool busy);

    QStackedWidget* pages_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QTreeWidget* infoTree_ = nullptr;
    QLabel* keyframeTitle_ = nullptr;
    QTreeWidget* keyframeList_ = nullptr;
    QAction* openAction_ = nullptr;

    QThread* probeThread_ = nullptr;
    std::atomic<bool> cancelProbe_{false};
};

// Testo con le versioni di Qt e FFmpeg, usato nella barra di stato e in "Informazioni".
QString versionSummary();

}  // namespace editor::app
