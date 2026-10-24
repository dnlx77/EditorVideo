#pragma once

#include "EditorCore/Sequence.h"

#include <string>
#include <vector>

namespace editor::core {

// Una clip audio: un tratto di un file (audio, o l'audio di un video) messo su una traccia audio in un punto della
// sequenza. A differenza dei pezzi video la clip sta dove la si mette: ha la sua posizione nella sequenza.
struct AudioClip
{
    int id = 0;            // identificativo stabile (non cambia spostando la clip, nemmeno su un'altra traccia)
    std::string path;      // UTF-8
    double start = 0.0;    // nel file
    double end = 0.0;
    double position = 0.0;  // dove inizia nella sequenza, in secondi
    double sourceDuration = 0.0;  // durata del file (0 = non nota: la fine si può allungare senza limite)

    double length() const { return end - start; }
    double endPosition() const { return position + length(); }
    bool operator==(const AudioClip&) const = default;
};

// Livello e muto/solo di una traccia audio.
struct AudioMix
{
    double levelDb = 0.0;  // volume della traccia, in dB (0 = com'è nel file)
    bool muted = false;
    bool solo = false;

    bool operator==(const AudioMix&) const = default;
};

// Una traccia audio aggiunta (A2, A3, ...): le sue clip in ordine di posizione, senza sovrapposizioni.
struct AudioTrack
{
    int id = 0;  // identificativo stabile (il nome A2, A3... dipende invece dalla posizione fra le tracce)
    AudioMix mix;
    std::vector<AudioClip> clips;

    bool operator==(const AudioTrack&) const = default;
};

// Le tracce audio del montaggio. La traccia A1 è l'audio dei pezzi video (attaccato a loro, vedi Sequence): qui ha
// solo il suo livello e muto/solo. Le altre (A2, A3, ...) hanno clip audio libere. Le tracce udibili si sommano (mix):
// se una traccia è in "solo" si sentono solo quelle in solo.
//
// Le clip si spostano, si dividono e si accorciano dai bordi con le stesse regole dei pezzi video (Sequence): sulla
// stessa traccia non si sovrappongono mai.
class AudioTracks
{
public:
    static constexpr int kOriginalTrack = 0;  // id della traccia A1 (l'audio dei pezzi video)
    static constexpr double kMinLevelDb = -60.0;
    static constexpr double kMaxLevelDb = 12.0;

    // Tracce. addTrack aggiunge una traccia vuota in fondo e ne restituisce l'id; removeTrack la toglie con le sue clip.
    int addTrack();
    bool removeTrack(int trackId);
    const std::vector<AudioTrack>& tracks() const { return tracks_; }
    const AudioTrack* track(int trackId) const;
    int trackIndex(int trackId) const;  // -1 se non c'è (A1 compresa: non è fra tracks())
    std::string trackName(int trackId) const;  // "A1", "A2"...; vuoto se non c'è

    // Livello e muto/solo (kOriginalTrack = A1). Il livello si limita a [kMinLevelDb, kMaxLevelDb].
    const AudioMix& mix(int trackId) const;  // di A1 se la traccia non c'è
    bool setLevel(int trackId, double db);
    bool setMuted(int trackId, bool muted);
    bool setSolo(int trackId, bool solo);
    // Se la traccia si sente nel mix (muto e solo di tutte le tracce compresi).
    bool audible(int trackId) const;
    // Quanto si moltiplica il segnale della traccia nel mix: 0 se non si sente.
    double gain(int trackId) const;
    static double dbToGain(double db);

    // Clip. add mette la clip sulla traccia (l'id della clip passata non conta) e ne restituisce l'id; 0 se la traccia
    // non c'è, la clip è vuota, inizia prima di 0 o si sovrappone a un'altra.
    int add(int trackId, AudioClip clip);
    bool remove(int clipId);
    const AudioClip* find(int clipId) const;
    int trackOf(int clipId) const;  // id della traccia della clip, -1 se non c'è

    // Dove può iniziare la clip sulla traccia `trackId` se la si vuole a `desired`: il punto libero più vicino (vedi
    // Sequence::nearestFreeStart). -1 se la clip o la traccia non c'è.
    double nearestFreePosition(int clipId, int trackId, double desired) const;
    // Sposta la clip a `position` sulla traccia `trackId` (anche un'altra); false (e nessuna modifica) se si
    // sovrapporrebbe ad altre clip o se la traccia non c'è.
    bool moveTo(int clipId, int trackId, double position);

    // Divide la clip della traccia che si trova al tempo `seconds` della sequenza, proprio lì. Restituisce l'id della
    // seconda parte, 0 se lì non c'è una clip (o si è sul suo bordo).
    int split(int trackId, double seconds);

    // Bordi, come Sequence::trimLimit/trim: Edge::Start sposta l'inizio nel file (la fine resta ferma nella sequenza),
    // Edge::End la fine. Senza ripple ci si ferma contro le clip vicine; con ripple la clip resta dove inizia e quelle
    // dopo sulla stessa traccia scorrono della differenza. La clip resta dentro il file e lunga almeno minLength.
    double trimLimit(int clipId, Sequence::Edge edge, double desired, double minLength, bool ripple) const;
    bool trim(int clipId, Sequence::Edge edge, double value, double minLength, bool ripple);

    // Toglie [from, to) della sequenza dalle clip della traccia (lì resta silenzio; le clip non scorrono). Una clip
    // tagliata nel mezzo diventa due. true se qualcosa è cambiato.
    bool removeRange(int trackId, double from, double to);

    // File: tutte le clip di `path` usano `newPath` (file ritrovato altrove) o vengono tolte. Restituiscono quante.
    int relocate(const std::string& path, const std::string& newPath);
    int removePath(const std::string& path);

    // Rimette tutto così com'è (es. riaprendo un progetto): tracce e clip ricevono id nuovi; le clip di ogni traccia
    // si mettono in ordine di posizione.
    void restore(AudioMix original, std::vector<AudioTrack> tracks);
    // Torna allo stato di `snapshot` (Annulla / Ripeti); gli id nuovi non riprendono quelli usati nel frattempo.
    void restoreEdits(const AudioTracks& snapshot);
    void clear();

    // Nessuna traccia aggiunta e A1 com'è all'inizio.
    bool isDefault() const { return tracks_.empty() && original_ == AudioMix{}; }
    double endTime() const;  // fine dell'ultima clip, 0 senza clip

    bool operator==(const AudioTracks& other) const
    {
        return original_ == other.original_ && tracks_ == other.tracks_;
    }

private:
    AudioTrack* mutableTrack(int trackId);
    AudioMix* mutableMix(int trackId);
    // La clip con id `clipId`: traccia e posizione nell'elenco; false se non c'è.
    bool locate(int clipId, int& trackIndex, int& clipIndex) const;
    // Se [position, position + length) si sovrappone a una clip della traccia diversa da `ignoredClip`.
    static bool overlaps(const AudioTrack& track, double position, double length, int ignoredClip);
    static void sortClips(AudioTrack& track);

    AudioMix original_;
    std::vector<AudioTrack> tracks_;
    int nextTrackId_ = 1;
    int nextClipId_ = 1;
};

}  // namespace editor::core
