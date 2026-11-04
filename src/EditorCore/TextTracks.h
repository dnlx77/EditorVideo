#pragma once

#include "EditorCore/Transform.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace editor::core {

// Colore con trasparenza, 8 bit per canale.
struct TextColor
{
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;

    bool operator==(const TextColor&) const = default;
};

enum class TextAlign
{
    Left,
    Center,
    Right,
};

// Come si vede una scritta. Grandezze e posizioni sono in frazioni del fotogramma, così il risultato è lo stesso a
// qualsiasi risoluzione.
struct TextStyle
{
    std::string fontFamily = "Segoe UI";  // UTF-8
    double size = 0.06;  // altezza del carattere, in frazione dell'altezza del fotogramma
    bool bold = false;
    bool italic = false;
    bool underline = false;
    TextAlign align = TextAlign::Center;
    double lineSpacing = 1.0;  // moltiplica l'interlinea normale
    TextColor color;
    double opacity = 1.0;  // 0..1, di tutta la scritta

    // Contorno e ombra; la larghezza/distanza è in frazione dell'altezza del fotogramma.
    bool outline = false;
    TextColor outlineColor{0, 0, 0, 255};
    double outlineWidth = 0.004;
    bool shadow = false;
    TextColor shadowColor{0, 0, 0, 160};
    double shadowOffsetX = 0.003;
    double shadowOffsetY = 0.003;

    // Riquadro dietro il testo.
    bool background = false;
    TextColor backgroundColor{0, 0, 0, 160};
    double backgroundPadding = 0.01;  // in frazione dell'altezza del fotogramma

    // Dove sta il centro del riquadro del testo: 0..1 della larghezza e dell'altezza del fotogramma.
    double centerX = 0.5;
    double centerY = 0.88;

    bool operator==(const TextStyle&) const = default;
};

// Una scritta: un testo con il suo stile, messa su un livello in un punto della sequenza.
struct TextClip
{
    int id = 0;  // identificativo stabile (non cambia spostando la scritta, nemmeno su un altro livello)
    double position = 0.0;  // dove inizia nella sequenza, in secondi
    double length = 5.0;
    std::string text;  // UTF-8, con '\n' fra le righe
    TextStyle style;

    // Dissolvenza in entrata e in uscita (secondi): l'opacità sale da 0 a 1 nei primi `fadeIn` secondi e scende da 1 a 0
    // negli ultimi `fadeOut`. Se insieme sono più lunghe della scritta si accorciano in proporzione.
    double fadeIn = 0.0;
    double fadeOut = 0.0;

    double endPosition() const { return position + length; }
    // Quanto si vede la scritta al tempo `t` della sequenza per le dissolvenze: 0..1 (1 = piena).
    double fadeFactor(double t) const
    {
        double in = std::max(fadeIn, 0.0);
        double out = std::max(fadeOut, 0.0);
        if (in + out > length && in + out > 0.0) {
            const double scale = length / (in + out);
            in *= scale;
            out *= scale;
        }
        double factor = 1.0;
        if (in > 0.0 && t < position + in)
            factor = std::min(factor, (t - position) / in);
        if (out > 0.0 && t > endPosition() - out)
            factor = std::min(factor, (endPosition() - t) / out);
        return std::clamp(factor, 0.0, 1.0);
    }
    bool operator==(const TextClip&) const = default;
};

// Uno stile con un nome ("Titolo", "Sottotitolo"), da riusare su altre scritte.
struct NamedTextStyle
{
    std::string name;
    TextStyle style;

    bool operator==(const NamedTextStyle&) const = default;
};

// Un pezzo di video su un livello sopra V1: una parte di un file messa in un punto della sequenza (come un Fragment di V1, ma
// con la sua posizione: i livelli sopra V1 non hanno la catena di pezzi attaccati).
struct LayerPiece
{
    int id = 0;      // identificativo stabile (lo stesso insieme degli id delle scritte: mai due uguali)
    int clipId = 0;  // file di origine (vedi Project)
    double start = 0.0;  // nel file di origine
    double end = 0.0;
    double position = 0.0;  // dove inizia nella sequenza, in secondi
    Transform transform;

    double length() const { return end - start; }
    double endPosition() const { return position + length(); }
    bool operator==(const LayerPiece&) const = default;
};

// Un livello video sopra V1 (V2, V3, ...): le sue scritte e i suoi pezzi di video, tutti insieme in ordine di posizione e mai
// sovrapposti fra loro (una scritta e un pezzo sullo stesso livello non stanno nello stesso istante).
struct TextLayer
{
    int id = 0;  // identificativo stabile (il nome V2, V3... dipende dalla posizione fra i livelli)
    std::vector<TextClip> clips;
    std::vector<LayerPiece> pieces;

    bool operator==(const TextLayer&) const = default;
};

// I livelli di scritte del montaggio, sopra il video. Il primo livello è il più basso: nell'anteprima e
// nell'esportazione un livello più alto copre quelli sotto. Sullo stesso livello le scritte non si sovrappongono mai.
class TextTracks
{
public:
    static constexpr double kMinLength = 0.1;  // una scritta non è più corta di così (secondi)

    // Livelli. addLayer ne aggiunge uno vuoto in cima e restituisce l'id.
    int addLayer();
    bool removeLayer(int layerId);  // con le sue scritte
    const std::vector<TextLayer>& layers() const { return layers_; }
    const TextLayer* layer(int layerId) const;
    int layerIndex(int layerId) const;  // -1 se non c'è
    std::string layerName(int layerId) const;  // "V2", "V3"... (V1 è la sequenza principale); vuoto se non c'è
    bool empty() const;  // nessuna scritta e nessun pezzo in nessun livello

    // Scritte. add mette la scritta sul livello (l'id passato non conta) e ne restituisce l'id; 0 se il livello non
    // c'è, la scritta è più corta di kMinLength, inizia prima di 0 o si sovrappone a un'altra del livello.
    int add(int layerId, TextClip clip);
    bool remove(int clipId);
    const TextClip* find(int clipId) const;
    int layerOf(int clipId) const;  // id del livello, -1 se la scritta non c'è

    // Dove può iniziare la scritta sul livello `layerId` se la si vuole a `desired`: il punto libero più vicino
    // (`desired` se lì ci sta, altrimenti contro la scritta che lo blocca, o in un altro spazio se è più vicino). -1 se
    // la scritta o il livello non ci sono.
    double nearestFreeStart(int clipId, int layerId, double desired) const;
    // Lo stesso per un oggetto che ancora non c'è, lungo `length` (ignoreId: un oggetto da non contare, 0 = nessuno).
    double freeStartFor(int layerId, double length, double desired, int ignoreId = 0) const;
    // Sposta la scritta sul livello `layerId` in modo che inizi a `start` (entro quello che nearestFreeStart
    // permette). false, e nessuna modifica, se `start` non è un posto libero.
    bool moveTo(int clipId, int layerId, double start);

    // Il posto [start, start + length) sul livello è libero (per mettere una scritta nuova).
    bool canPlace(int layerId, double start, double length) const;
    // Dove può stare il bordo se lo si vuole a `desired` (tempo della sequenza): fra la scritta prima e quella dopo, non
    // prima di 0 e lasciando alla scritta almeno kMinLength. -1 se la scritta non c'è.
    double edgeLimit(int clipId, bool atStart, double desired) const;

    // Bordi: sposta l'inizio (la fine resta dov'è) o la fine (l'inizio resta). Non si va sotto kMinLength, prima di 0 o
    // contro un'altra scritta del livello. false, e nessuna modifica, se non si può.
    bool setStart(int clipId, double start);
    bool setEnd(int clipId, double end);
    // Divide la scritta al tempo `seconds` della sequenza: la seconda parte ha lo stesso testo e stile. Restituisce
    // l'id della seconda parte, 0 se `seconds` non è dentro la scritta (con kMinLength per parte).
    int split(int clipId, double seconds);

    // Pezzi di video sui livelli. addPiece lo mette sul livello (l'id passato non conta) e ne restituisce l'id; 0 se il livello non
    // c'è, il pezzo è più corto di kMinLength, inizia prima di 0 o si sovrappone a un'altra scritta o a un altro pezzo del livello.
    int addPiece(int layerId, LayerPiece piece);
    bool removePiece(int pieceId);
    const LayerPiece* findPiece(int pieceId) const;
    int layerOfPiece(int pieceId) const;  // id del livello, -1 se il pezzo non c'è
    // Sposta il pezzo sul livello `layerId` in modo che inizi a `start` (come moveTo per le scritte).
    bool movePieceTo(int pieceId, int layerId, double start);
    // Cambia i bordi di un pezzo: dove inizia nella sequenza e la parte del file (start < end). false, e nessuna modifica, se il
    // pezzo diventa più corto di kMinLength, inizia prima di 0 o si sovrappone a un altro oggetto del livello.
    bool trimPiece(int pieceId, double position, double sourceStart, double sourceEnd);
    // Divide il pezzo al tempo `seconds` della sequenza; restituisce l'id della seconda parte, 0 se non è dentro il pezzo (con
    // kMinLength per parte).
    int splitPiece(int pieceId, double seconds);
    bool setPieceTransform(int pieceId, const Transform& transform);  // false se è già quella
    // I pezzi visibili al tempo `seconds`, dal livello più basso al più alto.
    std::vector<const LayerPiece*> piecesVisibleAt(double seconds) const;

    bool setText(int clipId, std::string text);
    bool setStyle(int clipId, const TextStyle& style);
    bool setLength(int clipId, double length);  // allunga/accorcia la fine; come setEnd
    // Le dissolvenze (secondi, da 0 alla durata della scritta); false se sono già quelle.
    bool setFades(int clipId, double fadeIn, double fadeOut);

    // Le scritte visibili al tempo `seconds`, dal livello più basso al più alto (per disegnarle in questo ordine).
    std::vector<const TextClip*> visibleAt(double seconds) const;
    // Fine dell'ultima scritta (0 se non ce ne sono).
    double endTime() const;

    void clear();
    // Rimette i livelli così come sono (riaprendo un progetto): ricevono id nuovi; le scritte non valide si scartano.
    void restore(std::vector<TextLayer> layers);
    // Torna allo stato di `snapshot` (Annulla / Ripeti); gli id nuovi non riprendono quelli usati nel frattempo.
    void restoreEdits(const TextTracks& snapshot);

    bool operator==(const TextTracks& other) const { return layers_ == other.layers_; }

private:
    TextClip* findMutable(int clipId);
    LayerPiece* findPieceMutable(int pieceId);
    // Dove sta una scritta o un pezzo ([inizio, fine) nella sequenza) e il livello che lo contiene (-1 se l'id non c'è).
    bool extentOf(int id, double& position, double& end) const;
    int layerOfObject(int id) const;
    // Gli intervalli [inizio, fine) occupati sul livello da scritte e pezzi, senza quello con id `ignoreId`, in ordine.
    std::vector<std::pair<double, double>> occupied(const TextLayer& layer, int ignoreId) const;
    // Il posto [start, end) è libero sul livello, ignorando la scritta `ignoreId`.
    bool isFree(const TextLayer& layer, double start, double end, int ignoreId) const;
    void sortLayer(TextLayer& layer) const;

    std::vector<TextLayer> layers_;
    int nextLayerId_ = 1;
    int nextClipId_ = 1;
};

}  // namespace editor::core
