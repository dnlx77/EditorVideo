#pragma once

#include <QtWidgets/QWidget>

#include <array>
#include <functional>
#include <map>
#include <vector>

class QHBoxLayout;

namespace editor::app {

class MixerStrip;

// Il pannello Mixer: una colonna per ogni traccia audio (A1, A2, ...) con fader in dB, muto e solo. Non conosce il
// progetto: chi lo usa gli dà le tracce (setTracks) e riceve i cambi dell'utente dalle funzioni onLevel/onMuted/onSolo.
class MixerPanel : public QWidget
{
public:
    struct Track
    {
        int trackId = 0;
        QString name;
        double levelDb = 0.0;
        bool muted = false;
        bool solo = false;
        bool audible = true;  // se la traccia si sente nel mix (muto e solo di tutte le tracce compresi)
        double pan = 0.0;     // da -1 (sinistra) a +1 (destra)
    };

    explicit MixerPanel(QWidget* parent = nullptr);

    // Mette le tracce. Se sono le stesse (id e nomi) si aggiornano solo i valori, senza toccare la colonna che l'utente
    // sta trascinando.
    void setTracks(const std::vector<Track>& tracks);
    // I picchi (0..1, sinistra e destra) di ogni traccia nell'ultimo intervallo di `seconds` secondi (AudioMixer::takePeaks):
    // le barre salgono subito e scendono piano. Una traccia senza picchi scende verso il silenzio.
    void feedPeaks(const std::map<int, std::array<float, 2>>& peaks, double seconds);
    void resetMeters();                    // tutte a zero (riproduzione ferma)
    double meterDb(int index, int channel) const;  // livello mostrato, in dB
    int stripCount() const { return static_cast<int>(strips_.size()); }
    const Track& trackAt(int index) const { return tracks_[static_cast<std::size_t>(index)]; }  // com'è mostrata

    std::function<void(int trackId, double db)> onLevel;
    std::function<void(int trackId, bool muted)> onMuted;
    std::function<void(int trackId, bool solo)> onSolo;
    std::function<void(int trackId, double pan)> onPan;

private:
    QHBoxLayout* layout_ = nullptr;
    std::vector<MixerStrip*> strips_;
    std::vector<Track> tracks_;
};

}  // namespace editor::app
