#pragma once

namespace editor::app {

// Imposta in EditorCore il lettore di immagini di riserva fatto con Qt (QImageReader, orientamento EXIF applicato): serve per
// i formati che l'FFmpeg della build non legge (il PNG).
void installQtImageLoader();

}  // namespace editor::app
