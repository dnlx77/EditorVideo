#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/PropertiesPanel.h"
#include "SelfTests.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorCore/VideoDecoder.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtGui/QImage>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolButton>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace editor::app {

namespace core = editor::core;

namespace {

struct Bounds
{
    bool any = false;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
    int width() const { return any ? maxX - minX + 1 : 0; }
    int height() const { return any ? maxY - minY + 1 : 0; }
    double centerX() const { return (minX + maxX) / 2.0; }
    double centerY() const { return (minY + maxY) / 2.0; }
};

// Il rettangolo dei pixel rossi (R alto, G e B bassi) dell'immagine.
Bounds redBounds(const QImage& image)
{
    Bounds b;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            if (qRed(p) < 200 || qGreen(p) > 60 || qBlue(p) > 60)
                continue;
            if (!b.any) {
                b = {true, x, x, y, y};
            } else {
                b.minX = std::min(b.minX, x);
                b.maxX = std::max(b.maxX, x);
                b.minY = std::min(b.minY, y);
                b.maxY = std::max(b.maxY, y);
            }
        }
    }
    return b;
}

std::shared_ptr<core::VideoFrame> redFrame(int width, int height)
{
    auto frame = std::make_shared<core::VideoFrame>();
    frame->width = width;
    frame->height = height;
    frame->stride = width * 4;
    frame->pixels.assign(static_cast<std::size_t>(frame->stride) * height, 0);
    for (std::size_t i = 0; i < frame->pixels.size(); i += 4) {
        frame->pixels[i + 2] = 255;  // rosso (BGRA)
        frame->pixels[i + 3] = 255;
    }
    return frame;
}

template <typename T>
T* child(QWidget& parent, const char* name)
{
    return parent.findChild<T*>(QString::fromLatin1(name));
}

}  // namespace

// --transform-test: vedi SelfTests.h. Codice 0 se tutto è giusto, 3 se un controllo non riesce.
int runTransformTest(const QStringList&, qsizetype)
{
    SelfCheck check;
    std::printf("Prova del pannello Proprietà e delle trasformazioni\n");

    // --- Il pannello da solo.
    {
        PropertiesPanel panel;
        panel.resize(320, 700);
        check(!panel.showingTransform(), "all'inizio il pannello mostra un messaggio, non le trasformazioni");
        core::Transform t;
        panel.showTransform(t, 1);
        check(panel.showingTransform(), "con un pezzo scelto mostra le trasformazioni");

        core::Transform applied;
        std::string lastField;
        int edits = 0;
        panel.onTransformEdit = [&](const std::function<void(core::Transform&)>& edit, const std::string& field) {
            edit(applied);
            lastField = field;
            ++edits;
        };
        bool fitCalled = false;
        bool fillCalled = false;
        panel.onFit = [&](bool fill) { (fill ? fillCalled : fitCalled) = true; };

        auto* scaleX = child<QDoubleSpinBox>(panel, "scalaX");
        auto* scaleY = child<QDoubleSpinBox>(panel, "scalaY");
        auto* posX = child<QDoubleSpinBox>(panel, "posizioneX");
        auto* rotation = child<QDoubleSpinBox>(panel, "rotazione");
        auto* cropLeft = child<QDoubleSpinBox>(panel, "ritaglioSinistra");
        auto* opacity = child<QDoubleSpinBox>(panel, "opacita");
        check(scaleX && scaleY && posX && rotation && cropLeft && opacity, "tutti i campi ci sono");
        if (!(scaleX && scaleY && posX && rotation && cropLeft && opacity))
            return 3;
        check(std::abs(scaleX->value() - 100.0) < 1e-9 && std::abs(opacity->value() - 100.0) < 1e-9, "i valori di base sono 100%");

        scaleX->setValue(50.0);
        check(edits == 1 && lastField == "scala" && std::abs(applied.scaleX - 0.5) < 1e-9 && std::abs(applied.scaleY - 0.5) < 1e-9,
              "la scala proporzionale cambia larghezza e altezza insieme");
        auto* linked = panel.findChild<QCheckBox*>(QStringLiteral("proporzionale"));
        check(linked != nullptr && linked->isChecked(), "la catenella è chiusa all'inizio");
        if (linked != nullptr)
            linked->setChecked(false);
        scaleY->setValue(25.0);
        check(std::abs(applied.scaleY - 0.25) < 1e-9 && std::abs(applied.scaleX - 0.5) < 1e-9,
              "a catenella aperta cambia solo l'altezza");
        posX->setValue(30.0);
        check(lastField == "posizioneX" && std::abs(applied.posX - 30.0) < 1e-9, "la posizione X arriva in pixel");
        auto* rotationSlider = child<QSlider>(panel, "rotazione_cursore");
        check(rotationSlider != nullptr, "la rotazione ha il cursore");
        if (rotationSlider != nullptr)
            rotationSlider->setValue(45);
        check(std::abs(rotation->value() - 45.0) < 1e-9 && std::abs(applied.rotation - 45.0) < 1e-9,
              "muovere il cursore cambia il campo e la rotazione");
        cropLeft->setValue(25.0);
        check(lastField == "ritaglioSinistra" && std::abs(applied.cropLeft - 0.25) < 1e-9, "il ritaglio è una frazione (25% = 0,25)");
        opacity->setValue(40.0);
        check(std::abs(applied.opacity - 0.4) < 1e-9, "l'opacità è una frazione (40% = 0,4)");
        QToolButton* flip = nullptr;
        for (QToolButton* b : panel.findChildren<QToolButton*>()) {
            if (b->text().contains(QStringLiteral("↔")))
                flip = b;
        }
        check(flip != nullptr, "c'è il pulsante Specchia in orizzontale");
        if (flip != nullptr)
            flip->click();
        check(applied.flipHorizontal, "Specchia in orizzontale lo accende");
        QPushButton* all = nullptr;
        QPushButton* fit = nullptr;
        QPushButton* fill = nullptr;
        for (QPushButton* b : panel.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Ripristina tutto"))
                all = b;
            if (b->text() == QStringLiteral("Adatta"))
                fit = b;
            if (b->text() == QStringLiteral("Riempi"))
                fill = b;
        }
        check(all && fit && fill, "ci sono Adatta, Riempi e Ripristina tutto");
        if (all && fit && fill) {
            all->click();
            check(applied.isIdentity() && lastField == "tutto", "Ripristina tutto riporta i valori di base");
            fit->click();
            fill->click();
            check(fitCalled && fillCalled, "Adatta e Riempi arrivano a chi sta in ascolto");
        }

        // Mostrare nuovi valori non è una modifica.
        const int before = edits;
        core::Transform other;
        other.scaleX = other.scaleY = 2.0;
        other.cropTop = 0.1;
        other.rotation = -12.5;
        panel.showTransform(other, 3);
        check(edits == before, "mostrare valori nuovi non conta come modifica");
        check(std::abs(scaleX->value() - 200.0) < 1e-9 && std::abs(child<QDoubleSpinBox>(panel, "ritaglioAlto")->value() - 10.0) < 1e-9
                  && std::abs(rotation->value() + 12.5) < 1e-9,
              "i campi mostrano i valori nuovi");
        panel.showNothing(QStringLiteral("niente"));
        check(!panel.showingTransform(), "senza pezzo scelto torna il messaggio");

        // La pagina delle scritte: l'editor sta nel pannello e si alterna con le altre pagine.
        TextEditorPanel textPage;
        panel.setTextPage(&textPage);
        check(!panel.showingText(), "la pagina delle scritte non si vede finché non serve");
        core::TextClip clip;
        clip.id = 1;
        clip.text = "Ciao";
        clip.length = 2.0;
        int textEdits = 0;
        textPage.onEdited = [&](const core::TextClip&) { ++textEdits; };
        textPage.setClip(&clip, {});
        panel.showText();
        check(panel.showingText() && !panel.showingTransform() && textPage.isVisibleTo(&panel),
              "con una scritta scelta il pannello mostra il suo editor");
        if (auto* edit = textPage.findChild<QPlainTextEdit*>("testo"))
            edit->setPlainText(QStringLiteral("Altro"));
        check(textEdits > 0, "scrivere nell'editor nel pannello arriva a chi ascolta");
        panel.showTransform(other, 1);
        check(panel.showingTransform() && !panel.showingText(), "con un pezzo scelto torna la pagina delle trasformazioni");

        // La pagina delle transizioni: i controlli mostrano i valori e ogni modifica porta tutti i valori a chi ascolta.
        core::TransitionClip transition;
        transition.id = 7;
        transition.type = "dissolvenza";
        transition.duration = 1.5;
        transition.crossfadeAudio = false;
        int transitionEdits = 0;
        std::string transitionField;
        double lastDuration = 0.0;
        bool lastAudio = false;
        panel.onTransitionEdit = [&](const std::string&, const core::TransitionParams&, double duration, bool audio,
                                     const std::string& field) {
            ++transitionEdits;
            transitionField = field;
            lastDuration = duration;
            lastAudio = audio;
        };
        panel.showTransitionClip(transition, 0.1, 3.0);
        auto* durationBox = child<QDoubleSpinBox>(panel, "transizioneDurata");
        auto* audioBox = child<QCheckBox>(panel, "transizioneAudio");
        check(panel.showingTransitionClip() && !panel.showingTransform() && transitionEdits == 0,
              "scegliendo una transizione il pannello mostra la sua pagina (senza contare come modifica)");
        check(std::abs(durationBox->value() - 1.5) < 1e-9 && !audioBox->isChecked() && durationBox->maximum() == 3.0,
              "la pagina mostra durata, audio e limite della transizione");
        durationBox->setValue(2.25);
        check(transitionEdits == 1 && transitionField == "durata" && std::abs(lastDuration - 2.25) < 1e-9 && !lastAudio,
              "cambiare la durata arriva a chi ascolta");
        audioBox->setChecked(true);
        check(transitionEdits == 2 && transitionField == "audio" && lastAudio && std::abs(lastDuration - 2.25) < 1e-9,
              "la dissolvenza incrociata dell'audio arriva a chi ascolta");
        panel.showTransform(other, 1);
        check(panel.showingTransform() && !panel.showingTransitionClip(), "da una transizione si torna alle trasformazioni");
    }

    // --- Il fotogramma disegnato con la trasformazione (a pixel).
    {
        PlayerWidget player;
        player.resize(640, 520);
        player.show();
        QCoreApplication::processEvents();
        const QSize project(1280, 720);
        player.showExternalFrame(redFrame(640, 360));
        QCoreApplication::processEvents();
        const Bounds base = redBounds(player.grab().toImage());
        check(base.any && base.width() > 200, "senza trasformazione il fotogramma si vede (rosso)");
        const auto bounds = [&](const core::Transform& t) {
            player.setFrameTransform(t, project);
            QCoreApplication::processEvents();
            return redBounds(player.grab().toImage());
        };
        const auto closeTo = [](double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; };

        core::Transform half;
        half.scaleX = half.scaleY = 0.5;
        const Bounds small = bounds(half);
        check(small.any && closeTo(small.width(), base.width() / 2.0, 5) && closeTo(small.height(), base.height() / 2.0, 5)
                  && closeTo(small.centerX(), base.centerX(), 3) && closeTo(small.centerY(), base.centerY(), 3),
              "scala 50%: metà larghezza e altezza, centrato");

        core::Transform moved = half;
        moved.posX = 320.0;  // un quarto della larghezza del progetto
        const Bounds shifted = bounds(moved);
        check(shifted.any && closeTo(shifted.centerX() - small.centerX(), base.width() / 4.0, 5) && closeTo(shifted.centerY(), small.centerY(), 3),
              "posizione X +320 px: l'immagine va a destra di un quarto del fotogramma");

        core::Transform cropped;
        cropped.cropLeft = 0.5;
        const Bounds cut = bounds(cropped);
        check(cut.any && closeTo(cut.width(), base.width() / 2.0, 5) && closeTo(cut.height(), base.height(), 3)
                  && closeTo(cut.centerX(), base.centerX() + base.width() / 4.0, 4),
              "ritaglio sinistro 50%: resta la metà destra, dove stava");

        core::Transform turned;
        turned.rotation = 90.0;
        const Bounds rot = bounds(turned);
        check(rot.any && closeTo(rot.width(), base.height(), 5) && closeTo(rot.height(), base.height(), 5),
              "rotazione 90°: l'immagine esce dal fotogramma in alto e in basso (ritagliata) ed è larga quanto era alta");

        core::Transform faded;
        faded.opacity = 0.5;
        player.setFrameTransform(faded, project);
        QCoreApplication::processEvents();
        const QImage fadedImage = player.grab().toImage();
        const QRgb center = fadedImage.pixel(static_cast<int>(base.centerX()), static_cast<int>(base.centerY()));
        check(qRed(center) > 110 && qRed(center) < 146 && qGreen(center) < 30, "opacità 50%: rosso a metà sul nero");

        const Bounds again = bounds(core::Transform{});
        check(again.any && again.width() == base.width() && again.height() == base.height() && closeTo(again.centerX(), base.centerX(), 1),
              "tornando ai valori di base il disegno è quello di prima");
    }

    // --- Il pannello nella finestra.
    {
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        QDockWidget* dock = window.propertiesDock();
        check(dock != nullptr && window.propertiesPanel() != nullptr && dock->widget() == window.propertiesPanel(),
              "la finestra ha il pannello Proprietà");
        check(dock != nullptr && dock->windowTitle() == QStringLiteral("Proprietà") && !dock->isFloating() && dock->isVisible(),
              "è agganciato, visibile e si chiama Proprietà");
        check(dock != nullptr && dock->geometry().left() >= window.programDock()->geometry().right() - 2,
              "sta all'estremo destro, dopo il Montaggio");
        check(window.propertiesPanel() != nullptr && !window.propertiesPanel()->showingTransform(),
              "senza pezzi scelti non mostra le trasformazioni");
    }

    std::printf(check.failures() == 0 ? "Tutte le prove sono riuscite.\n" : "Prove non riuscite: %d\n", check.failures());
    return check.failures() == 0 ? 0 : 3;
}

}  // namespace editor::app
