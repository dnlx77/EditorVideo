#pragma once

#include <QtWidgets/QWidget>

#include <functional>
#include <utility>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QMenu;
class QSlider;
class QToolButton;

namespace editor::app {

// Barra dei comandi di riproduzione di un'anteprima, con i pulsanti classici: inizio, segno precedente, fotogramma
// indietro, riproduci/pausa, stop, fotogramma avanti, segno successivo, fine; poi velocità, ripetizione e volume.
// La Sorgente ha in più IN, OUT, vai a IN / OUT e "aggiungi al montaggio". I "segni" sono i fotogrammi chiave nella
// Sorgente e gli inizi dei pezzi nel Montaggio.
//
// La barra non fa niente da sola: ogni pulsante chiama la sua funzione (onPlayPause, ...), e chi la usa le dice lo
// stato (setPlaying, setSpeed, ...). Se lo spazio è poco nasconde prima i pulsanti meno importanti.
class TransportBar : public QWidget
{
public:
    enum class Kind
    {
        Source,
        Program,
    };

    explicit TransportBar(Kind kind, QWidget* parent = nullptr);

    // Un widget (di solito il tempo) fra i pulsanti di riproduzione e quelli di velocità e volume; prende lo spazio che
    // avanza.
    void setInfoWidget(QWidget* widget);

    void setMediaEnabled(bool enabled);
    void setPlaying(bool playing);
    void setSpeed(double speed);  // velocità attuale (negativa = all'indietro), scritta sul pulsante
    void setLoop(bool loop);
    void setVolume(int percent, bool muted);
    void setQuality(int divisor);  // qualità dell'anteprima mostrata nel menu (1, 2, 4, 8)

    std::function<void()> onStart;
    std::function<void()> onPreviousMark;
    std::function<void()> onPreviousFrame;
    std::function<void()> onPlayPause;
    std::function<void()> onStop;
    std::function<void()> onNextFrame;
    std::function<void()> onNextMark;
    std::function<void()> onEnd;
    // Solo Sorgente.
    std::function<void()> onMarkIn;
    std::function<void()> onMarkOut;
    std::function<void()> onGoToIn;
    std::function<void()> onGoToOut;
    std::function<void()> onAdd;
    // Velocità scelta dal menu (positiva), ripetizione, volume (0-100) e muto.
    std::function<void(double)> onSpeedChosen;
    std::function<void(bool)> onLoopToggled;
    std::function<void(int)> onVolumeChanged;
    std::function<void(bool)> onMuteToggled;
    // Qualità dell'anteprima scelta dal menu a tendina: 1 = originale, 2, 4, 8 = frazione della risoluzione (proxy).
    std::function<void(int)> onQualityChosen;

    QSize minimumSizeHint() const override;

    // Simboli dei pulsanti, disegnati nel colore del testo del tema (le icone standard sono nere, sul tema scuro non si
    // vedono).
    enum class Glyph
    {
        SkipBack,
        SeekBack,
        FrameBack,
        Play,
        Pause,
        Stop,
        FrameForward,
        SeekForward,
        SkipForward,
        Volume,
        Muted,
    };

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QToolButton* addButton(QHBoxLayout* layout, int priority, const QString& tip, std::function<void()>* action);
    void setGlyph(QToolButton* button, Glyph glyph);
    void updateIcons();  // ridisegna i simboli (es. il tema è cambiato)
    void updateVisibility();  // nasconde i pulsanti che non ci stanno, dal meno importante

    struct Item
    {
        QWidget* widget = nullptr;
        int priority = 0;  // più alto = resta visibile più a lungo
    };
    std::vector<Item> items_;
    std::vector<std::pair<QToolButton*, Glyph>> glyphs_;
    QWidget* info_ = nullptr;
    QHBoxLayout* infoSlot_ = nullptr;
    QToolButton* play_ = nullptr;
    QToolButton* speed_ = nullptr;
    QMenu* speedMenu_ = nullptr;
    QComboBox* quality_ = nullptr;
    QToolButton* loop_ = nullptr;
    QToolButton* mute_ = nullptr;
    QSlider* volume_ = nullptr;
    bool playing_ = false;
    bool muted_ = false;
};

}  // namespace editor::app
