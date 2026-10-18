#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace editor::app {

// Uscita audio PCM 16 bit sulla scheda audio predefinita di Windows (waveOut).
// write() e flush() vanno chiamate da un solo thread (quello di riproduzione);
// playedFrames() e drained() si possono leggere da un altro thread.
// open(), reset() e close() solo quando nessun thread sta scrivendo.
class AudioOutput
{
public:
    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // false se non c'è una scheda audio utilizzabile.
    bool open(int sampleRate, int channels);
    void close();
    bool isOpen() const;
    int sampleRate() const;

    // Accoda campioni interlacciati; si blocca finché c'è spazio. Restituisce false se `stop` diventa true.
    bool write(const std::int16_t* samples, std::size_t count, const std::atomic<bool>& stop);
    // Invia anche il blocco riempito solo in parte (a fine file).
    void flush();

    // Campioni per canale già suonati dall'ultimo reset(): è l'orologio della riproduzione.
    long long playedFrames() const;
    // true se tutto ciò che è stato accodato è già stato suonato.
    bool drained() const;

    // Interrompe subito il suono e scarta l'audio accodato; la posizione torna a 0.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace editor::app
