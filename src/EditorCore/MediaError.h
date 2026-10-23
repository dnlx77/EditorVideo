#pragma once

#include <stdexcept>

namespace editor::core {

// Errore su un file multimediale, con un messaggio in italiano da mostrare all'utente.
class MediaError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Il file non ha un flusso del tipo richiesto (es. un file solo audio aperto come video).
class NoStreamError : public MediaError
{
public:
    using MediaError::MediaError;
};

}  // namespace editor::core
