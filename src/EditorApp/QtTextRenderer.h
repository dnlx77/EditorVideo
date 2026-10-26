#pragma once

#include "EditorCore/TextImage.h"

namespace editor::app {

// Disegna le scritte con Qt (font di Windows, contorno, ombra, sfondo): lo stesso disegno serve per l'anteprima e per
// l'esportazione, così coincidono. Va usato dopo aver creato QGuiApplication (i font si leggono da lì).
class QtTextRenderer : public core::TextRenderer
{
public:
    core::TextImage render(const core::TextClip& clip, int frameWidth, int frameHeight) const override;
};

}  // namespace editor::app
