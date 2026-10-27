#pragma once

namespace editor::app {

// Il tipo di dati che l'elenco delle transizioni mette nel trascinamento: l'id della transizione (vedi
// core::Transition::id), in UTF-8. La linea del tempo del montaggio lo accetta sulle giunzioni fra due pezzi.
inline constexpr const char* kTransitionMime = "application/x-editorvideo-transition";

}  // namespace editor::app
