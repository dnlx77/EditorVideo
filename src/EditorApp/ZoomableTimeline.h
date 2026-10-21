#pragma once

#include "EditorCore/TimelineZoom.h"

#include <QtWidgets/QWidget>

class QScrollBar;
class QToolButton;

namespace editor::app {

// Base comune delle linee del tempo con lo zoom orizzontale (Sorgente e Montaggio).
// - Ctrl+rotellina ingrandisce o riduce tenendo fermo il tempo sotto il mouse; la rotellina da sola (o con Maiusc)
//   scorre in orizzontale.
// - In basso una riga con la barra di scorrimento (solo quando la vista è ingrandita) e il pulsante "Adatta", che
//   torna a mostrare tutto.
// - Durante la riproduzione la vista segue il cursore una pagina alla volta; se l'utente scorre a mano smette di
//   seguirlo finché la riproduzione non riparte o si preme "Adatta".
// La classe derivata dice dove sta l'asse del tempo (timeAxis), ridisegna quando la vista cambia (viewChanged) e
// converte con xForTime / timeForX, che tengono conto di zoom e scorrimento.
class ZoomableTimeline : public QWidget
{
public:
    void zoomIn();   // centrato sul cursore se si vede, altrimenti al centro
    void zoomOut();
    void fitAll();   // "Adatta"
    void zoomAtX(double factor, double widgetX);
    bool isZoomed() const { return !zoom_.isFit(); }
    const core::TimelineZoom& zoom() const { return zoom_; }
    // Zoom massimo: circa 12 pixel per fotogramma.
    void setFrameDuration(double seconds);
    // La riproduzione è (ri)partita: la vista torna a seguire il cursore.
    void resumeFollowing();
    bool isFollowing() const { return following_; }

protected:
    explicit ZoomableTimeline(QWidget* parent = nullptr);

    static constexpr int kControlsHeight = 18;  // riga della barra di scorrimento e di "Adatta", in basso

    // La parte del widget che rappresenta il tempo: conta solo la sua estensione orizzontale.
    virtual QRectF timeAxis() const = 0;
    // Zoom o scorrimento cambiati: tutto va ridisegnato.
    virtual void viewChanged() = 0;
    // Dove sta il cursore di riproduzione (per centrare lo zoom da tastiera).
    virtual double playheadTime() const = 0;

    void setTimelineDuration(double seconds);
    // Il cursore si è spostato: se la vista lo segue e lui è uscito dalla parte visibile, si gira pagina.
    void followPlayhead(double seconds);
    double xForTime(double seconds) const;  // coordinate del widget
    double timeForX(double x) const;
    // Parte orizzontale visibile dell'asse, per non disegnare fuori (con zoom alto l'asse è larghissimo).
    double visibleLeft() const { return timeAxis().left(); }
    double visibleRight() const { return timeAxis().right(); }

    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void syncControls();  // barra di scorrimento e pulsante dopo un cambiamento della vista
    void userScrolled();  // l'utente ha scorso a mano: la vista smette di seguire il cursore

    core::TimelineZoom zoom_;
    QScrollBar* scroll_ = nullptr;
    QToolButton* fit_ = nullptr;
    bool following_ = true;
    bool syncing_ = false;  // si sta aggiornando la barra: i suoi segnali non sono dell'utente
};

}  // namespace editor::app
