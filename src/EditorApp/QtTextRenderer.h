#pragma once

#include "EditorCore/TextImage.h"

#include <map>

namespace editor::app {

// Disegna le scritte con Qt (font di Windows, contorno, ombra, sfondo): lo stesso disegno serve per l'anteprima e per
// l'esportazione, così coincidono. Va usato dopo aver creato QGuiApplication (i font si leggono da lì).
class QtTextRenderer : public core::TextRenderer
{
public:
    core::TextImage render(const core::TextClip& clip, int frameWidth, int frameHeight) const override;
};

// Le scritte di un montaggio già disegnate, alla misura dei fotogrammi del progetto, per l'esportazione: Qt disegna nel
// thread dell'interfaccia (dove si crea questo oggetto), l'esportazione, che gira in un altro thread, le prende pronte.
// Con `draw` false non disegna niente (restituisce immagini vuote): basta per calcolare l'anteprima dell'esportazione,
// che guarda solo quando ci sono le scritte.
class PrerenderedTextRenderer : public core::TextRenderer
{
public:
    PrerenderedTextRenderer(const core::TextTracks& tracks, int frameWidth, int frameHeight, bool draw);
    core::TextImage render(const core::TextClip& clip, int frameWidth, int frameHeight) const override;

private:
    std::map<int, core::TextImage> images_;  // per id di scritta
};

}  // namespace editor::app
