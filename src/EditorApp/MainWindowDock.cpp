#include "EditorApp/MainWindow.h"

#include "EditorApp/MainWindowShared.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/ProxyManager.h"
#include "EditorApp/UiText.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TimelineWidget.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/FileDrop.h"
#include "EditorCore/Encoder.h"
#include "EditorCore/LibraryInfo.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/MixExport.h"
#include "EditorCore/Proxy.h"
#include "EditorCore/SegmentExport.h"
#include "EditorApp/ThumbnailCache.h"
#include "EditorApp/WaveformStore.h"
#include "EditorCore/TimeUtil.h"
#include "EditorCore/Waveform.h"

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QCloseEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QCursor>
#include <QtGui/QHoverEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QStyle>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QHelpEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <thread>


namespace editor::app {
using namespace detail;

// ---------------------------------------------------------------------------------------------------------------
// Pannelli

void MainWindow::resetLayout()
{
    if (fullScreenDock_ != nullptr)
        exitFullScreen();
    // A sinistra i file, al centro Sorgente e Montaggio affiancati, sotto la linea del tempo del montaggio.
    for (QDockWidget* dock : docks()) {
        dock->setFloating(false);
        removeDockWidget(dock);
    }
    addDockWidget(Qt::TopDockWidgetArea, filesDock_);
    splitDockWidget(filesDock_, sourceDock_, Qt::Horizontal);
    splitDockWidget(sourceDock_, programDock_, Qt::Horizontal);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
    for (QDockWidget* dock : docks())
        dock->show();
    resizeDocks({filesDock_, sourceDock_, programDock_}, {320, 590, 590}, Qt::Horizontal);
    resizeDocks({sourceDock_, timelineDock_}, {620, 230}, Qt::Vertical);
    // La linea del tempo alta quella iniziale anche se la finestra non ha posto per 620 + 230 (il resto va sopra).
    timelineHeight_ = 230;
    timelineRedocking_ = true;
    QTimer::singleShot(0, this, [this] { applyTimelineHeight(); });
}

void MainWindow::saveLayout()
{
    QSettings s = settings();
    s.setValue(QStringLiteral("finestra/geometria"), saveGeometry());
    s.setValue(QStringLiteral("finestra/disposizione"), saveState());
    s.setValue(QStringLiteral("finestra/altezzaLineaDelTempo"), timelineHeight_);
}

void MainWindow::restoreLayout()
{
    QSettings s = settings();
    const QByteArray geometry = s.value(QStringLiteral("finestra/geometria")).toByteArray();
    const QByteArray state = s.value(QStringLiteral("finestra/disposizione")).toByteArray();
    timelineHeight_ = std::max(230, s.value(QStringLiteral("finestra/altezzaLineaDelTempo"), 230).toInt());
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    if (!state.isEmpty() && !restoreState(state))
        resetLayout();
    // Una disposizione salvata prima che la linea del tempo potesse stare solo in basso: la si rimette lì.
    if (!timelineDock_->isFloating() && dockWidgetArea(timelineDock_) != Qt::BottomDockWidgetArea)
        redockTimeline();
    // Il monitor dove stava la finestra (o un pannello staccato) potrebbe non esserci più.
    keepOnScreen(this);
    for (QDockWidget* dock : docks()) {
        if (dock->isFloating())
            keepOnScreen(dock);
    }
}

void MainWindow::keepOnScreen(QWidget* window)
{
    const QRect frame = window->frameGeometry();
    for (const QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(frame.adjusted(40, 10, -40, -10)))
            return;
    }
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    QSize size = window->size().boundedTo(available.size() - QSize(40, 60));
    window->resize(size);
    window->move(available.center() - QPoint(size.width() / 2, size.height() / 2));
}

QDockWidget* MainWindow::dockedPanelAt(const QPoint& pos) const
{
    for (QDockWidget* dock : docks()) {
        if (dock->isVisible() && !dock->isFloating() && dock->geometry().contains(pos))
            return dock;
    }
    return nullptr;
}

bool MainWindow::findCorner(const QPoint& pos, CornerDrag& corner) const
{
    // Un incrocio è un punto fra i pannelli (non dentro uno) con pannelli diversi a sinistra e a destra (separatore
    // verticale) e sopra e sotto (separatore orizzontale), entro qualche pixel.
    constexpr int d = 7;
    if (dockedPanelAt(pos) != nullptr)
        return false;
    QDockWidget* tl = dockedPanelAt(pos + QPoint(-d, -d));
    QDockWidget* tr = dockedPanelAt(pos + QPoint(d, -d));
    QDockWidget* bl = dockedPanelAt(pos + QPoint(-d, d));
    QDockWidget* br = dockedPanelAt(pos + QPoint(d, d));
    CornerDrag found;
    if (tl != nullptr && tr != nullptr && tl != tr) {
        found.left = tl;
        found.right = tr;
    } else if (bl != nullptr && br != nullptr && bl != br) {
        found.left = bl;
        found.right = br;
    }
    if (tl != nullptr && bl != nullptr && tl != bl) {
        found.top = tl;
        found.bottom = bl;
    } else if (tr != nullptr && br != nullptr && tr != br) {
        found.top = tr;
        found.bottom = br;
    }
    if (found.left == nullptr || found.top == nullptr)
        return false;
    found.start = pos;
    found.leftWidth = found.left->width();
    found.rightWidth = found.right->width();
    found.topHeight = found.top->height();
    found.bottomHeight = found.bottom->height();
    corner = found;
    return true;
}

bool MainWindow::event(QEvent* event)
{
    // Incroci fra separatori: QMainWindow sposta un separatore alla volta; qui, trascinando il punto in cui due si
    // incrociano, si spostano tutti e due (larghezza e altezza insieme).
    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = mouse->position().toPoint();
        if (cornerDrag_) {
            const QPoint delta = pos - cornerDrag_->start;
            resizeDocks({cornerDrag_->left, cornerDrag_->right},
                        {std::max(1, cornerDrag_->leftWidth + delta.x()), std::max(1, cornerDrag_->rightWidth - delta.x())},
                        Qt::Horizontal);
            resizeDocks({cornerDrag_->top, cornerDrag_->bottom},
                        {std::max(1, cornerDrag_->topHeight + delta.y()), std::max(1, cornerDrag_->bottomHeight - delta.y())},
                        Qt::Vertical);
            return true;
        }
        CornerDrag corner;
        const bool onCorner = mouse->buttons() == Qt::NoButton && findCorner(pos, corner);
        if (onCorner != cornerCursor_) {
            cornerCursor_ = onCorner;
            if (onCorner)
                setCursor(Qt::SizeAllCursor);
            else
                unsetCursor();
        }
        if (onCorner)
            return true;
        break;
    }
    case QEvent::MouseButtonPress: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        CornerDrag corner;
        if (mouse->button() == Qt::LeftButton && findCorner(mouse->position().toPoint(), corner)) {
            cornerDrag_ = corner;
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        if (cornerDrag_) {
            cornerDrag_.reset();
            return true;
        }
        break;
    case QEvent::Leave:
        if (cornerCursor_ && !cornerDrag_) {
            cornerCursor_ = false;
            unsetCursor();
        }
        break;
    default:
        break;
    }
    return QMainWindow::event(event);
}

void MainWindow::redockTimeline()
{
    if (fullScreenDock_ == timelineDock_)
        exitFullScreen();
    if (debugDock())
        std::fprintf(stderr, "redock: floating %d area %d w %d\n", timelineDock_->isFloating(),
                     static_cast<int>(dockWidgetArea(timelineDock_)), timelineDock_->width());
    timelinePoll_->stop();
    showDockPreview(false);
    timelineRedocking_ = true;
    // Sempre la stessa sequenza, qualunque sia lo stato di prima: tolta, aggiunta in basso, agganciata, visibile.
    // Mai una finestra nativa da agganciata (vedi startTimelineDrag).
    timelineDock_->setAttribute(Qt::WA_NativeWindow, false);
    removeDockWidget(timelineDock_);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_, Qt::Horizontal);
    timelineDock_->setFloating(false);
    timelineDock_->show();
    // L'altezza si sistema dopo che Qt ha rifatto la disposizione: subito verrebbe ignorata.
    QTimer::singleShot(0, this, [this] { applyTimelineHeight(); });
}

void MainWindow::applyTimelineHeight()
{
    // La disposizione si ricalcola subito, prima di dare l'altezza (e dopo si ridisegna tutto).
    if (layout() != nullptr) {
        layout()->invalidate();
        layout()->activate();
    }
    // L'ultima altezza da agganciata, almeno quella iniziale e al massimo metà della finestra.
    const int maximum = std::max(230, height() / 2);
    const int wanted = std::clamp(timelineHeight_, 230, maximum);
    // Con il solo pannello in basso Qt non sa da chi togliere lo spazio: gli si dà anche un pannello di sopra, che
    // prende il resto.
    QDockWidget* above = nullptr;
    for (QDockWidget* dock : {sourceDock_, programDock_, filesDock_}) {
        if (dock->isVisible() && !dock->isFloating()) {
            above = dock;
            break;
        }
    }
    if (above != nullptr) {
        const int total = above->height() + timelineDock_->height();
        resizeDocks({above, timelineDock_}, {std::max(1, total - wanted), wanted}, Qt::Vertical);
    } else {
        resizeDocks({timelineDock_}, {wanted}, Qt::Vertical);
    }
    if (layout() != nullptr)
        layout()->activate();
    update();
    timelineRedocking_ = false;
    timelineHeight_ = timelineDock_->height();
}

bool MainWindow::isTitleHighlighted(QDockWidget* dock) const
{
    return dock->property("titoloEvidenziato").toBool();
}

void MainWindow::setTitleHighlight(QDockWidget* dock, bool highlighted)
{
    if (isTitleHighlighted(dock) == highlighted)
        return;
    dock->setProperty("titoloEvidenziato", highlighted);
    // Il foglio di stile guarda la proprietà: va riapplicato.
    dock->style()->unpolish(dock);
    dock->style()->polish(dock);
    dock->update();
    if (highlighted)
        dock->setCursor(Qt::OpenHandCursor);
    else
        dock->unsetCursor();
}

bool MainWindow::updateTitleHover(QDockWidget* dock, QEvent* event)
{
    const auto inTitle = [dock](const QPointF& pos) {
        // Il titolo è la striscia sopra il contenuto del pannello.
        return !dock->isFloating() && dock->widget() != nullptr && pos.y() >= 0 && pos.y() < dock->widget()->geometry().top();
    };
    switch (event->type()) {
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        setTitleHighlight(dock, inTitle(static_cast<QHoverEvent*>(event)->position()));
        return true;
    case QEvent::HoverLeave:
        setTitleHighlight(dock, false);
        return true;
    case QEvent::MouseButtonPress:
        if (isTitleHighlighted(dock) && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
            dock->setCursor(Qt::ClosedHandCursor);  // si trascina
        return true;
    case QEvent::MouseButtonRelease:
        if (isTitleHighlighted(dock))
            dock->setCursor(Qt::OpenHandCursor);
        return true;
    default:
        return false;
    }
}

void MainWindow::timelinePollStep(bool buttonDown, const QPoint& cursor)
{
    if (!timelineDock_->isFloating() || fullScreenDock_ != nullptr) {
        timelinePoll_->stop();
        showDockPreview(false);
        return;
    }
    if (buttonDown && !pollButtonDown_)
        pollPressPos_ = timelineDock_->pos();  // inizio di un trascinamento (o di un clic qualsiasi)
    const QRect window = frameGeometry();
    const bool low = window.contains(cursor) && cursor.y() >= window.top() + window.height() / 2;
    // Spostata: lo dice Windows (ha fatto lui lo spostamento) oppure la posizione (prove senza Windows).
    const bool moved = movedByWindows_ || timelineDock_->pos() != pollPressPos_;
    // Durante il trascinamento, con il cursore nella metà bassa: si vede dove andrà.
    showDockPreview(buttonDown && moved && low);
    // Rilasciato dopo averla spostata: è stata lasciata lì.
    const bool released = !buttonDown && pollButtonDown_ && moved;
    pollButtonDown_ = buttonDown;
    if (!buttonDown)
        movedByWindows_ = false;
    if (released) {
        showDockPreview(false);
        // Il pulsante si è alzato, ma chi sta trascinando (Qt, o Windows) può non averlo ancora saputo: si aspetta
        // che abbia finito, altrimenti alla fine del suo trascinamento la staccherebbe di nuovo.
        QTimer::singleShot(30, this, [this, cursor] { finishTimelineDrop(cursor, 40); });
    }
}

QPoint MainWindow::cursorNow() const
{
    return testCursor_ ? *testCursor_ : QCursor::pos();
}

void MainWindow::startTimelineDrag(const QPoint& global, const QPoint& local)
{
    // Staccata qui, larga al massimo due terzi della finestra, con il punto preso sotto il cursore; poi lo spostamento
    // lo fa Windows (come trascinando il titolo di una finestra), che a fine spostamento manda WM_EXITSIZEMOVE.
    const int w = std::min(timelineDock_->width(), std::max(420, width() * 2 / 3));
    const int h = timelineDock_->height();
    timelineDock_->setFloating(true);
    timelineDock_->resize(w, h);
    const int grabX = std::clamp(local.x(), 40, w - 40);
    timelineDock_->move(global - QPoint(grabX, 12));
    timelineDock_->show();
    QCoreApplication::processEvents();  // la finestra nativa deve esistere ed essere al suo posto
    // L'handle della finestra (staccata, quindi di primo livello) senza QWidget::winId(), che renderebbe il pannello una
    // finestra nativa per sempre: riagganciato resterebbe una finestra figlia di Windows, ridisegnata e messa al suo posto
    // solo quando la finestra principale si riorganizza.
    if (timelineDock_->windowHandle() == nullptr)
        return;
    void* hwnd = reinterpret_cast<void*>(timelineDock_->windowHandle()->winId());
    if (nativeDragForTest_) {
        nativeDragForTest_(hwnd, global);
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);  // in pixel fisici, come vuole Windows
    ReleaseCapture();       // il mouse non è più di Qt: lo prende Windows
    SendMessageW(static_cast<HWND>(hwnd), WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
}

void MainWindow::finishTimelineDrop(const QPoint& cursor, int attempts)
{
    if (debugDock())
        std::fprintf(stderr, "finish: floating %d grabber %p buttons %d moveloop %d area %d w %d\n", timelineDock_->isFloating(),
                     static_cast<void*>(QWidget::mouseGrabber()), static_cast<int>(QGuiApplication::mouseButtons()),
                     nativeMoveLoop_, static_cast<int>(dockWidgetArea(timelineDock_)), timelineDock_->width());
    if (!timelineDock_->isFloating() || fullScreenDock_ != nullptr)
        return;
    const bool dragging = nativeMoveLoop_ || QWidget::mouseGrabber() != nullptr;
    if (dragging) {
        if (attempts > 0)
            QTimer::singleShot(25, this, [this, cursor, attempts] { finishTimelineDrop(cursor, attempts - 1); });
        return;
    }
    if (!redockTimelineIfDroppedLow(cursor, false))
        return;
    // Controllo: se qualcosa l'ha staccata di nuovo subito dopo, si riprova (qualche volta).
    QTimer::singleShot(80, this, [this, cursor, attempts] {
        if (timelineDock_->isFloating() && attempts > 0)
            finishTimelineDrop(cursor, attempts - 1);
    });
}

void MainWindow::showDockPreview(bool visible)
{
    if (!visible) {
        if (dockPreview_ != nullptr)
            dockPreview_->hide();
        return;
    }
    if (dockPreview_ == nullptr) {
        dockPreview_ = new QWidget(this);
        dockPreview_->setAttribute(Qt::WA_TransparentForMouseEvents);
        dockPreview_->setAttribute(Qt::WA_StyledBackground);
        QColor color = palette().color(QPalette::Highlight);
        dockPreview_->setStyleSheet(QStringLiteral("background: rgba(%1, %2, %3, 90); border: 3px solid rgb(%1, %2, %3);")
                                        .arg(color.red())
                                        .arg(color.green())
                                        .arg(color.blue()));
    }
    // La striscia in basso, a tutta larghezza, alta quanto sarà la linea del tempo.
    const int bottom = statusBar()->isVisible() ? statusBar()->geometry().top() : height();
    const int h = std::clamp(timelineHeight_, 230, std::max(230, height() / 2));
    dockPreview_->setGeometry(0, std::max(0, bottom - h), width(), std::min(h, bottom));
    dockPreview_->raise();
    dockPreview_->show();
}

bool MainWindow::redockTimelineIfDroppedLow(const QPoint& cursor, bool buttonDown)
{
    if (buttonDown || !timelineDock_->isFloating() || fullScreenDock_ != nullptr)
        return false;
    const QRect window = frameGeometry();
    if (!window.contains(cursor) || cursor.y() < window.top() + window.height() / 2)
        return false;
    redockTimeline();
    statusBar()->showMessage(QStringLiteral("Linea del tempo riagganciata in basso."), 4000);
    return true;
}

void MainWindow::toggleFullScreen(QDockWidget* dock)
{
    if (fullScreenDock_ != nullptr) {
        const bool same = fullScreenDock_ == dock;
        exitFullScreen();
        if (same)
            return;
    }
    // Il pannello diventa una finestra a sé, a schermo intero e senza la barra del titolo; uscendo si ritrova la
    // disposizione di prima.
    layoutBeforeFullScreen_ = saveState();
    fullScreenDock_ = dock;
    dock->setFloating(true);
    dock->setTitleBarWidget(new QWidget(dock));
    dock->showFullScreen();
    dock->activateWindow();
    statusBar()->showMessage(QStringLiteral("Schermo intero: Esc o doppio clic sull'immagine per uscire."), 5000);
}

void MainWindow::exitFullScreen()
{
    QDockWidget* dock = fullScreenDock_;
    if (dock == nullptr)
        return;
    fullScreenDock_ = nullptr;
    QWidget* title = dock->titleBarWidget();
    dock->setTitleBarWidget(nullptr);
    delete title;
    dock->showNormal();
    restoreState(layoutBeforeFullScreen_);
    activateWindow();
}

}  // namespace editor::app
