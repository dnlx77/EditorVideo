#pragma once

#include <QtWidgets/QMainWindow>

class QLabel;

namespace editor::app {

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void createMenus();
    void openFile();
    void showAbout();

    QLabel* placeholder_ = nullptr;
};

// Testo con le versioni di Qt e FFmpeg, usato nella barra di stato e in "Informazioni".
QString versionSummary();

}  // namespace editor::app
