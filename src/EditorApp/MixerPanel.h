#pragma once

#include <QtWidgets/QWidget>

#include <functional>
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
    };

    explicit MixerPanel(QWidget* parent = nullptr);

    // Mette le tracce. Se sono le stesse (id e nomi) si aggiornano solo i valori, senza toccare la colonna che l'utente
    // sta trascinando.
    void setTracks(const std::vector<Track>& tracks);
    int stripCount() const { return static_cast<int>(strips_.size()); }
    const Track& trackAt(int index) const { return tracks_[static_cast<std::size_t>(index)]; }  // com'è mostrata

    std::function<void(int trackId, double db)> onLevel;
    std::function<void(int trackId, bool muted)> onMuted;
    std::function<void(int trackId, bool solo)> onSolo;

private:
    QHBoxLayout* layout_ = nullptr;
    std::vector<MixerStrip*> strips_;
    std::vector<Track> tracks_;
};

}  // namespace editor::app
