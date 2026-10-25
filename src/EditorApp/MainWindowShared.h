#pragma once

// Funzioni e costanti usate da più file della finestra principale (MainWindow*.cpp).

#include "EditorApp/MainWindow.h"
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

namespace editor::app::detail {

inline QTreeWidgetItem* addRow(QTreeWidgetItem* parent, const QString& name, const QString& value)
{
    auto* item = new QTreeWidgetItem(parent, {name, value});
    return item;
}

inline QString streamTitle(const core::StreamInfo& stream)
{
    QString title = QStringLiteral("Flusso %1 - %2").arg(stream.index).arg(qs(core::streamKindName(stream.kind)));
    if (stream.attachedPicture)
        title = QStringLiteral("Flusso %1 - Copertina").arg(stream.index);
    if (!stream.language.empty())
        title += QStringLiteral(" (%1)").arg(qs(stream.language));
    return title;
}

inline QString codecText(const core::StreamInfo& stream)
{
    QString text = qs(stream.codec);
    if (!stream.profile.empty())
        text += QStringLiteral(", profilo %1").arg(qs(stream.profile));
    if (!stream.codecLongName.empty())
        text += QStringLiteral("  (%1)").arg(qs(stream.codecLongName));
    return text;
}

inline void addStream(QTreeWidget* tree, const core::StreamInfo& stream)
{
    auto* item = new QTreeWidgetItem(tree, {streamTitle(stream), qs(stream.codec)});
    addRow(item, QStringLiteral("Codec"), codecText(stream));

    switch (stream.kind) {
    case core::StreamKind::Video:
        addRow(item, QStringLiteral("Risoluzione"), QStringLiteral("%1 x %2").arg(stream.width).arg(stream.height));
        addRow(item, QStringLiteral("Frequenza fotogrammi"), qs(core::formatFrameRate(stream.frameRate)));
        addRow(item, QStringLiteral("Formato pixel"),
               stream.pixelFormat.empty() ? QStringLiteral("sconosciuto") : qs(stream.pixelFormat));
        break;
    case core::StreamKind::Audio:
        addRow(item, QStringLiteral("Frequenza di campionamento"), QStringLiteral("%1 Hz").arg(stream.sampleRate));
        addRow(item, QStringLiteral("Canali"),
               stream.channelLayout.empty() ? QString::number(stream.channels)
                                            : QStringLiteral("%1 (%2)").arg(stream.channels).arg(qs(stream.channelLayout)));
        break;
    default:
        break;
    }

    if (stream.kind == core::StreamKind::Video || stream.kind == core::StreamKind::Audio)
        addRow(item, QStringLiteral("Bitrate"), qs(core::formatBitRate(stream.bitRate)));
    if (stream.durationSeconds >= 0)
        addRow(item, QStringLiteral("Durata"), qs(core::formatDuration(stream.durationSeconds)));
    item->setExpanded(true);
}

// Risultato dell'analisi passato dal thread di lavoro alla finestra.
struct ProbeOutcome
{
    std::optional<core::MediaInfo> info;
    QString error;
    bool cancelled = false;
};

// Una clip audio non diventa più corta di così accorciandola dai bordi.
inline constexpr double kMinAudioClip = 0.05;

inline const QString kAudioFilter = QStringLiteral(
    "File audio (*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus *.wma *.ac3 *.mka);;"
    "Audio di un video (*.mp4 *.mkv *.mov *.avi *.webm);;Tutti i file (*.*)");

inline const QString kVideoFilter = QStringLiteral(
    "File video e audio (*.mp4 *.mkv *.mov *.avi *.m4v *.webm *.ts *.mts *.m2ts *.mpg *.wmv *.flv "
    "*.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus);;Tutti i file (*.*)");

inline const QString kProjectExtension = QString::fromLatin1(core::kProjectExtension);  // ".evproj"
inline const QString kProjectFilter = QStringLiteral("Progetto EditorVideo (*%1)").arg(kProjectExtension);
inline constexpr int kRecentProjects = 5;
inline const QString kRecentProjectsKey = QStringLiteral("progettiRecenti");  // impostazione con i progetti recenti

// Titolo del pannello di un'anteprima: con il triangolo se è quella attiva (lì vanno Spazio e frecce).
inline QString previewTitle(const QString& name, bool active)
{
    return active ? QStringLiteral("▶ ") + name : name;
}

inline QSettings settings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("EditorVideo"), QStringLiteral("EditorVideo"));
}

// Cartella di lavoro: lì si aprono il salvataggio dei progetti e dell'esportazione e vanno i proxy dei progetti mai
// salvati. Di solito "Documenti\Editor video project"; si cambia da File > Cartella di lavoro. Si crea se manca.
inline QString workFolder()
{
    QString folder = settings().value(QStringLiteral("cartellaDiLavoro")).toString();
    if (folder.isEmpty())
        folder = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Editor video project"));
    QDir().mkpath(folder);
    return QDir::toNativeSeparators(folder);
}

// Il doppio clic sul titolo di una finestra staccata è di Windows (la barra del titolo è sua) e a Qt non arriva come
// evento che si possa intercettare: lo si prende fra i messaggi di Windows, prima che Windows lo gestisca da sé.
// EDITORVIDEO_DEBUG_DOCK=1: scrive su stderr cosa succede staccando e riagganciando la linea del tempo.
inline bool debugDock()
{
    static const bool enabled = qEnvironmentVariableIntValue("EDITORVIDEO_DEBUG_DOCK") != 0;
    return enabled;
}

class TitleDoubleClickFilter : public QAbstractNativeEventFilter
{
public:
    TitleDoubleClickFilter(std::function<bool(void* hwnd)> onDoubleClick, std::function<void(void* hwnd, bool moving)> onMoveLoop)
        : onDoubleClick_(std::move(onDoubleClick))
        , onMoveLoop_(std::move(onMoveLoop))
    {
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
    {
        if (eventType != "windows_generic_MSG")
            return false;
        const auto* msg = static_cast<const MSG*>(message);
        // Inizio e fine dello spostamento di una finestra da parte di Windows (trascinando il suo titolo).
        if (debugDock() && (msg->message == WM_ENTERSIZEMOVE || msg->message == WM_EXITSIZEMOVE || msg->message == WM_MOVE))
            std::fprintf(stderr, "native %x hwnd %p\n", msg->message, static_cast<void*>(msg->hwnd));
        if (msg->message == WM_ENTERSIZEMOVE || msg->message == WM_EXITSIZEMOVE) {
            onMoveLoop_(msg->hwnd, msg->message == WM_ENTERSIZEMOVE);
            return false;
        }
        if (msg->message != WM_NCLBUTTONDBLCLK || msg->wParam != HTCAPTION || !onDoubleClick_(msg->hwnd))
            return false;
        if (result != nullptr)  // per i messaggi dalla coda Qt può non passarlo
            *result = 0;
        return true;  // gestito: Windows non deve fare altro
    }

private:
    std::function<bool(void* hwnd)> onDoubleClick_;
    std::function<void(void* hwnd, bool moving)> onMoveLoop_;
};

// Elenco dei file del progetto: i file si possono trascinare sulla linea del tempo, come da Esplora file (sulla corsia
// video vanno nel montaggio, su una traccia audio aggiunta diventano clip audio).
class ClipListWidget : public QListWidget
{
public:
    std::function<QString(int clipId)> pathOf;

protected:
    QStringList mimeTypes() const override { return {QStringLiteral("text/uri-list")}; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* mime = new QMimeData;
        QList<QUrl> urls;
        for (const QListWidgetItem* item : items) {
            const QString path = pathOf ? pathOf(item->data(Qt::UserRole).toInt()) : QString();
            if (!path.isEmpty())
                urls.append(QUrl::fromLocalFile(path));
        }
        mime->setUrls(urls);
        return mime;
    }
};

}  // namespace editor::app::detail
