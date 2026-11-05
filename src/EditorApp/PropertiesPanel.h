#pragma once

#include "EditorCore/Animation.h"
#include "EditorCore/Transform.h"
#include "EditorCore/TransitionList.h"
#include "EditorCore/Transitions.h"

#include <QtWidgets/QWidget>

#include <array>
#include <functional>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace editor::app {

// Il pannello "Proprietà", agganciato a destra: il contenuto dipende dall'oggetto scelto sulla linea del tempo. Per ora ha
// tre pagine: nessun oggetto (un messaggio), un pezzo video (le sue trasformazioni: scala, posizione, rotazione, ritaglio,
// aspetto) e una scritta (il suo editor). Le altre (transizioni) si aggiungono come pagine.
class PropertiesPanel : public QWidget
{
public:
    explicit PropertiesPanel(QWidget* parent = nullptr);

    // Nessuna pagina utile: mostra `message`.
    void showNothing(const QString& message);
    // La pagina delle trasformazioni, con i valori di `transform` (del pezzo "principale"). `count` = quanti pezzi video
    // sono scelti: se più di uno, ogni modifica vale per tutti (solo il campo cambiato). I controlli già uguali non si
    // toccano.
    // `animation`: lo stato dei fotogrammi chiave di ogni gruppo di parametri sul cursore (vedi core::groupState); `enabled`
    // falso (più pezzi scelti, o cursore fuori dal pezzo) spegne i rombi.
    struct AnimationState
    {
        bool enabled = false;
        std::array<core::GroupState, core::kAnimGroupCount> groups{};
    };
    void showTransform(const core::Transform& transform, int count);
    void showTransform(const core::Transform& transform, int count, const AnimationState& animation);
    bool showingTransform() const;
    // La pagina delle scritte (l'editor, già costruito): il pannello ne prende possesso. showText() la mostra.
    void setTextPage(QWidget* page);
    void showText();
    bool showingText() const;
    // La pagina di una transizione: tipo, direzione, durata (fra `minDuration` e `maxDuration`) e dissolvenza incrociata
    // dell'audio. I controlli già uguali non si toccano.
    void showTransitionClip(const core::TransitionClip& clip, double minDuration, double maxDuration);
    bool showingTransitionClip() const;
    // I valori com'erano all'ultimo showTransform più le modifiche fatte dopo nei controlli.
    core::Transform transform() const;

    // Un campo è stato cambiato: `edit` modifica una Transform (si applica a ogni pezzo scelto, lasciando gli altri campi);
    // `field` è il nome del campo (per unire le modifiche di seguito dello stesso campo in una voce di Annulla). Il
    // progetto non usa moc: niente segnali propri.
    std::function<void(const std::function<void(core::Transform&)>& edit, const std::string& field)> onTransformEdit;
    // I comandi dei fotogrammi chiave di un gruppo (scala, posizione, rotazione, ritaglio, opacità): il rombo (mette o toglie la
    // chiave sul cursore, o fa partire l'animazione), le frecce (vanno alla chiave prima/dopo), il ripristino (spegne
    // l'animazione del gruppo; ResetAll: di tutti, il gruppo è ignorato).
    enum class AnimAction
    {
        ToggleKey,
        PrevKey,
        NextKey,
        Reset,
        ResetAll,
    };
    std::function<void(core::AnimGroup group, AnimAction action)> onAnimationAction;
    // "Adatta" (false) o "Riempi" (true): scala 100% / scala che riempie il fotogramma, con posizione e ritaglio a zero.
    std::function<void(bool fill)> onFit;
    // Un controllo della transizione è cambiato: i valori che ora hanno i controlli (tutti) e il nome del campo cambiato.
    std::function<void(const std::string& type, const core::TransitionParams& params, double duration, bool crossfadeAudio,
                       const std::string& field)>
        onTransitionEdit;

private:
    struct Row
    {
        QDoubleSpinBox* spin = nullptr;
        QSlider* slider = nullptr;
    };
    Row addRow(QVBoxLayout* into, const QString& label, double min, double max, double step, int decimals, const QString& suffix,
               int sliderMin, int sliderMax, double sliderScale, const char* name);
    void edited(const std::string& field, std::function<void(core::Transform&)> edit);
    QWidget* buildTransformPage();
    QWidget* buildTransitionPage();
    void transitionEdited(const std::string& field);
    QToolButton* resetButton(const QString& tip, const std::string& field, core::AnimGroup group, std::function<void(core::Transform&)> reset);
    void animate(core::AnimGroup group, AnimAction action);

    QStackedWidget* stack_ = nullptr;
    QWidget* transformPage_ = nullptr;
    QWidget* textPage_ = nullptr;
    QWidget* transitionPage_ = nullptr;
    QComboBox* transitionType_ = nullptr;
    QComboBox* transitionDirection_ = nullptr;
    QDoubleSpinBox* transitionDuration_ = nullptr;
    QCheckBox* transitionAudio_ = nullptr;
    QLabel* transitionHint_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* countNote_ = nullptr;
    Row scaleX_;
    Row scaleY_;
    QCheckBox* linked_ = nullptr;
    Row posX_;
    Row posY_;
    Row rotation_;
    Row cropLeft_;
    Row cropRight_;
    Row cropTop_;
    Row cropBottom_;
    Row opacity_;
    QToolButton* flipH_ = nullptr;
    QToolButton* flipV_ = nullptr;
    struct AnimButtons
    {
        QToolButton* prev = nullptr;
        QToolButton* key = nullptr;
        QToolButton* next = nullptr;
    };
    std::array<AnimButtons, core::kAnimGroupCount> animButtons_{};
    std::array<bool, core::kAnimGroupCount> groupAnimated_{};
    core::Transform shown_;
    bool updating_ = false;
};

}  // namespace editor::app
