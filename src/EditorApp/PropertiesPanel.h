#pragma once

#include "EditorCore/Animation.h"
#include "EditorCore/Speed.h"
#include "EditorCore/Transform.h"
#include "EditorCore/TransitionList.h"
#include "EditorCore/Transitions.h"

#include "AnimMiniTimeline.h"

#include <QtWidgets/QWidget>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
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
        bool presets = false;  // c'è un pezzo scelto da solo: i preset di animazione si possono applicare
        std::array<core::GroupState, core::kAnimGroupCount> groups{};
    };
    void showTransform(const core::Transform& transform, int count);
    void showTransform(const core::Transform& transform, int count, const AnimationState& animation);
    // La mini linea del tempo delle chiavi del pezzo (vuota/nascosta senza animazione): MainWindow ne mette i dati e le azioni.
    AnimMiniTimeline* miniTimeline() const { return mini_; }
    // La pagina delle scritte con in più la sezione «Trasforma» (scala, posizione, rotazione, specchio, opacità, con rombi e mini
    // linea del tempo: gli stessi controlli dei pezzi video, senza ritaglio) in fondo all'editor della scritta. I valori sono
    // quelli della trasformazione della scritta sul cursore.
    // La scritta ha una sola posizione: quella vera nel fotogramma, in % (centro di stile + spostamento). `centerX/centerY` sono il
    // centro di stile (0..1) e `frameWidth/frameHeight` il fotogramma; la modifica di X o Y arriva a `onTextPosition`.
    void showTextTransform(const core::Transform& transform, const AnimationState& animation, double centerX, double centerY, int frameWidth,
                           int frameHeight);
    std::function<void(bool isX, double percent)> onTextPosition;
    // La pagina delle scritte (l'editor, già costruito): il pannello ne prende possesso. showText() la mostra.
    void setTextPage(QWidget* page);
    void showText();
    // La pagina di una transizione: tipo, direzione, durata (fra `minDuration` e `maxDuration`) e dissolvenza incrociata
    // dell'audio. I controlli già uguali non si toccano.
    void showTransitionClip(const core::TransitionClip& clip, double minDuration, double maxDuration);
    // La velocità degli oggetti scelti (in %: 100 = normale) e l'inverso. Dopo showTransform mostra la sezione «Velocità» in
    // cima alla pagina dei pezzi video; showAudioSpeed mostra una pagina con la sola velocità (clip audio e audio dei pezzi).
    // `speed` in secondi di file per secondo di montaggio (1 = normale).
    void showSpeed(double speed, bool reverse, const core::AudioSpeedOptions& options);
    void showAudioSpeed(double speed, bool reverse, const core::AudioSpeedOptions& options);
    // Un controllo della velocità è cambiato: il valore nuovo (velocità: 1 = normale) e/o il verso; l'altro non cambia.
    std::function<void(std::optional<double> speed, std::optional<bool> reverse)> onSpeedEdit;
    // «Mantieni intonazione» o «Silenzia se inverso» è cambiato (l'altro non cambia).
    std::function<void(std::optional<bool> keepPitch, std::optional<bool> muteWhenReversed)> onAudioSpeedEdit;
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
    // Un preset di animazione scelto dal menu "Animazione" (si applica a tutto il pezzo scelto).
    std::function<void(core::AnimPreset preset)> onAnimationPreset;
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
    // La sezione «Velocità» (valore in %, valori comuni, «Inverti»): ne servono due, nella pagina dei pezzi e in quella dell'audio.
    struct SpeedControls
    {
        QWidget* box = nullptr;
        QDoubleSpinBox* spin = nullptr;
        QCheckBox* reverse = nullptr;
        QCheckBox* keepPitch = nullptr;
        QCheckBox* muteReversed = nullptr;
    };
    SpeedControls buildSpeedBox();
    void setSpeedControls(SpeedControls& controls, double speed, bool reverse, const core::AudioSpeedOptions& options);
    void transitionEdited(const std::string& field);
    QToolButton* resetButton(const QString& tip, const std::string& field, core::AnimGroup group, std::function<void(core::Transform&)> reset);
    void animate(core::AnimGroup group, AnimAction action);
    void updateTransformControls(const core::Transform& transform, int count, const AnimationState& animation);
    // I controlli della trasformazione stanno in un solo widget (body_): nella pagina dei pezzi o in fondo all'editor della scritta.
    void attachBody(bool toText);

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
    SpeedControls speedVideo_;
    SpeedControls speedAudio_;
    QWidget* audioPage_ = nullptr;
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
    std::vector<QAction*> panActions_;  // le panoramiche del menu dei preset
    QToolButton* presetButton_ = nullptr;
    QWidget* body_ = nullptr;           // i controlli della trasformazione
    QLabel* textHeading_ = nullptr;     // «Trasforma», solo nella pagina delle scritte
    QWidget* cropSeparator_ = nullptr;  // il ritaglio non c'è per le scritte
    QWidget* cropSection_ = nullptr;
    QPushButton* fitButton_ = nullptr;
    QPushButton* fillButton_ = nullptr;
    bool textMode_ = false;
    AnimMiniTimeline* mini_ = nullptr;
    core::Transform shown_;
    double textCenterX_ = 0.5;  // (solo per le scritte) centro di stile e fotogramma, per passare da % a spostamento
    double textCenterY_ = 0.5;
    int textFrameW_ = 1920;
    int textFrameH_ = 1080;
    bool updating_ = false;

private:
    // Solo per le prove (EditorVideoTests.exe e --smoke-test), che li raggiungono attraverso TestAccess (TestAccess.h).
    friend class TestAccess;
    bool showingTransform() const;
    bool showingText() const;
    bool showingTransitionClip() const;
    bool showingAudioSpeed() const;
};

}  // namespace editor::app
