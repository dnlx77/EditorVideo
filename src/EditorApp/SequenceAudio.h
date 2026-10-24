#pragma once

#include "EditorApp/AudioOutput.h"
#include "EditorCore/AudioMixer.h"

#include <atomic>
#include <map>
#include <memory>
#include <thread>

namespace editor::app {

// L'audio del montaggio: un thread che calcola il mix (core::AudioMixer) e lo manda alla scheda audio. Il suo orologio
// (quanto audio è già stato suonato) è quello della riproduzione del montaggio: il video lo segue, quindi l'audio non
// si interrompe ai cambi di pezzo. Si usa solo in avanti fino a 2x (core::audibleAtSpeed), con la stessa intonazione
// (TimeStretch).
class SequenceAudio
{
public:
    SequenceAudio();
    ~SequenceAudio();

    SequenceAudio(const SequenceAudio&) = delete;
    SequenceAudio& operator=(const SequenceAudio&) = delete;

    // Parte dal punto `from` (secondi della sequenza) alla velocità `speed` e si ferma da sola a `length`. false se
    // non c'è una scheda audio utilizzabile (o EDITORVIDEO_NO_AUDIO=1): allora la riproduzione va a tempo reale.
    bool start(core::AudioMixer mixer, double from, double length, double speed);
    void stop();
    bool running() const { return running_; }

    // Posizione di ciò che si sente adesso, in secondi della sequenza (si ferma a `length`). Da chiamare solo con
    // running().
    double position() const;

    // Volume dell'anteprima (0-1), e livelli delle tracce che cambiano mentre si suona.
    void setVolume(float gain) { volume_ = gain; }
    void setTrackGains(const std::map<int, float>& gains);
    void setSegmentEnvelopes(std::vector<std::vector<core::LevelPoint>> envelopes);  // le linee del livello dei tratti

private:
    void work();

    AudioOutput out_;
    bool opened_ = false;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::unique_ptr<core::AudioMixer> mixer_;
    double from_ = 0.0;
    double length_ = 0.0;
    double speed_ = 1.0;
    std::atomic<float> volume_{1.0f};
};

}  // namespace editor::app
