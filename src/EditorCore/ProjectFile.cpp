#include "EditorCore/ProjectFile.h"

#include "EditorCore/Json.h"
#include "EditorCore/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace editor::core {

namespace {

constexpr const char* kFormatName = "EditorVideo progetto";

std::filesystem::path projectDir(const std::string& projectPath)
{
    std::error_code ec;
    return std::filesystem::absolute(pathFromUtf8(projectPath), ec).parent_path();
}

// Percorso relativo alla cartella del progetto ("" se non si può, es. su un altro disco).
std::string relativeTo(const std::filesystem::path& dir, const std::string& path)
{
    const std::filesystem::path relative = pathFromUtf8(path).lexically_relative(dir);
    if (relative.empty())
        return {};
    return utf8FromPath(relative);
}

bool isFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

// Il file salvato: il percorso assoluto se c'è, altrimenti quello relativo alla cartella del progetto.
std::optional<std::string> locate(const std::filesystem::path& dir, const std::string& absolute, const std::string& relative)
{
    if (!absolute.empty() && isFile(pathFromUtf8(absolute)))
        return absolute;
    if (!relative.empty()) {
        const std::filesystem::path candidate = (dir / pathFromUtf8(relative)).lexically_normal();
        if (isFile(candidate))
            return utf8FromPath(candidate);
    }
    return std::nullopt;
}

std::string hexOf(const std::vector<std::uint8_t>& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (const std::uint8_t b : bytes) {
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text;
}

std::vector<std::uint8_t> bytesOf(const std::string& hex)
{
    const auto value = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = value(hex[i]);
        const int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return bytes;
}

// Il formato del progetto: un oggetto per flusso, con i parametri di codifica in esadecimale.
JsonValue formatToJson(const ProjectFormat& format)
{
    if (format.empty())
        return {};
    JsonValue::Array streams;
    for (const StreamSignature& s : format.streams) {
        JsonValue::Object stream{{"tipo", s.kind == StreamKind::Video ? "video" : "audio"},
                                 {"codec", s.codec},
                                 {"profilo", s.profile},
                                 {"parametri", hexOf(s.extradata)}};
        if (s.kind == StreamKind::Video) {
            stream["larghezza"] = s.width;
            stream["altezza"] = s.height;
            stream["formatoPixel"] = s.pixelFormat;
            stream["fotogrammiAlSecondo"] = s.frameRate;
        } else {
            stream["frequenza"] = s.sampleRate;
            stream["canali"] = s.channels;
            stream["disposizioneCanali"] = s.channelLayout;
        }
        streams.emplace_back(std::move(stream));
    }
    return JsonValue::Object{{"daFile", format.sourceName}, {"flussi", std::move(streams)}};
}

JsonValue ticks(double seconds)
{
    return secondsToTicks(seconds);
}

JsonValue optionalTicks(const std::optional<double>& seconds)
{
    return seconds ? JsonValue(secondsToTicks(*seconds)) : JsonValue();
}

double seconds(const JsonValue& value, const char* what)
{
    if (!value.isInteger())
        throw ProjectFileError(std::string("Il progetto è rovinato: manca o non è valido \"") + what + "\".");
    return ticksToSeconds(value.asInteger());
}

std::string text(const JsonValue& value)
{
    return value.isString() ? value.asString() : std::string();
}

JsonValue mixToJson(const AudioMix& mix)
{
    JsonValue::Object json{{"livello", mix.levelDb}, {"muto", mix.muted}, {"solo", mix.solo}};
    if (mix.pan != 0.0)
        json["pan"] = mix.pan;  // solo se non è al centro: i progetti senza pan restano uguali a prima
    if (!mix.effects.empty()) {
        JsonValue::Array effects;
        for (const AudioEffect& e : mix.effects) {
            static const char* const kTypes[] = {"tagliaBassi", "shelfBassi", "campana", "shelfAlti", "tagliaAlti"};
            if (e.kind == EffectKind::Normalize) {
                effects.emplace_back(JsonValue::Object{{"tipo", "normalizza"}, {"attivo", e.enabled}, {"livelloDb", e.targetDb}});
                continue;
            }
            JsonValue::Array bands;
            for (const EqBand& b : e.bands) {
                bands.emplace_back(JsonValue::Object{{"tipo", kTypes[static_cast<int>(b.type)]},
                                                     {"attiva", b.enabled},
                                                     {"frequenza", b.freq},
                                                     {"guadagno", b.gainDb},
                                                     {"q", b.q}});
            }
            effects.emplace_back(JsonValue::Object{{"tipo", "equalizzatore"}, {"attivo", e.enabled}, {"bande", std::move(bands)}});
        }
        json["effetti"] = std::move(effects);
    }
    return json;
}

// La linea del livello di un pezzo o di una clip: coppie [tempo nel file in tick, dB].
JsonValue transformToJson(const Transform& t)
{
    // Solo i valori diversi da quelli di base.
    const Transform base;
    JsonValue::Object o;
    if (t.scaleX != base.scaleX)
        o["scalaX"] = t.scaleX;
    if (t.scaleY != base.scaleY)
        o["scalaY"] = t.scaleY;
    if (t.posX != base.posX)
        o["posizioneX"] = t.posX;
    if (t.posY != base.posY)
        o["posizioneY"] = t.posY;
    if (t.rotation != base.rotation)
        o["rotazione"] = t.rotation;
    if (t.cropLeft != base.cropLeft)
        o["ritaglioSinistra"] = t.cropLeft;
    if (t.cropRight != base.cropRight)
        o["ritaglioDestra"] = t.cropRight;
    if (t.cropTop != base.cropTop)
        o["ritaglioAlto"] = t.cropTop;
    if (t.cropBottom != base.cropBottom)
        o["ritaglioBasso"] = t.cropBottom;
    if (t.flipHorizontal)
        o["specchioOrizzontale"] = true;
    if (t.flipVertical)
        o["specchioVerticale"] = true;
    if (t.opacity != base.opacity)
        o["opacita"] = t.opacity;
    return o;
}

const char* animKey(AnimParam p)
{
    switch (p) {
        case AnimParam::ScaleX: return "scalaX";
        case AnimParam::ScaleY: return "scalaY";
        case AnimParam::PosX: return "posizioneX";
        case AnimParam::PosY: return "posizioneY";
        case AnimParam::Rotation: return "rotazione";
        case AnimParam::CropLeft: return "ritaglioSinistra";
        case AnimParam::CropRight: return "ritaglioDestra";
        case AnimParam::CropTop: return "ritaglioAlto";
        case AnimParam::CropBottom: return "ritaglioBasso";
        case AnimParam::Opacity: return "opacita";
        case AnimParam::Count: break;
    }
    return "";
}

const char* interpName(Interp i)
{
    switch (i) {
        case Interp::Linear: return "lineare";
        case Interp::Hold: return "fermo";
        case Interp::Smooth: return "dolce";
        case Interp::Bezier: return "bezier";
    }
    return "lineare";
}

// Animazione: per ogni parametro animato una lista di chiavi {"t": tempo nel file (tick), "v": valore, "p": passaggio,
// "b": [c1x, c1y, c2x, c2y] se "bezier"}. I tempi sono quelli del file di origine, non della sequenza.
JsonValue animationToJson(const TransformAnimation& a)
{
    JsonValue::Object o;
    for (std::size_t i = 0; i < kAnimParamCount; ++i) {
        const AnimParam p = static_cast<AnimParam>(i);
        if (!a.animated(p))
            continue;
        JsonValue::Array keys;
        for (const AnimKey& k : a.track(p)) {
            JsonValue::Object key{{"t", ticks(k.time)}, {"v", k.value}};
            if (k.interp != Interp::Linear)
                key["p"] = std::string(interpName(k.interp));
            if (k.interp == Interp::Bezier)
                key["b"] = JsonValue::Array{k.c1x, k.c1y, k.c2x, k.c2y};
            keys.emplace_back(std::move(key));
        }
        o[animKey(p)] = std::move(keys);
    }
    return o;
}

TransformAnimation animationFromJson(const JsonValue& v)
{
    TransformAnimation a;
    if (!v.isObject())
        return a;
    for (std::size_t i = 0; i < kAnimParamCount; ++i) {
        const AnimParam p = static_cast<AnimParam>(i);
        const JsonValue& list = v[animKey(p)];
        if (!list.isArray())
            continue;
        for (const JsonValue& item : list.asArray()) {
            if (!item.isObject() || !item["t"].isNumber() || !item["v"].isNumber())
                continue;
            AnimKey k;
            k.time = ticksToSeconds(static_cast<std::int64_t>(item["t"].asNumber()));
            k.value = item["v"].asNumber();
            if (item["p"].isString()) {
                const std::string name = item["p"].asString();
                if (name == "fermo")
                    k.interp = Interp::Hold;
                else if (name == "dolce")
                    k.interp = Interp::Smooth;
                else if (name == "bezier")
                    k.interp = Interp::Bezier;
            }
            if (k.interp == Interp::Bezier && item["b"].isArray() && item["b"].asArray().size() == 4) {
                const JsonValue::Array& b = item["b"].asArray();
                if (b[0].isNumber() && b[1].isNumber() && b[2].isNumber() && b[3].isNumber()) {
                    k.c1x = b[0].asNumber();
                    k.c1y = b[1].asNumber();
                    k.c2x = b[2].asNumber();
                    k.c2y = b[3].asNumber();
                }
            }
            a.track(p).push_back(k);
        }
    }
    normalizeAnimation(a);
    return a;
}

Transform transformFromJson(const JsonValue& v)
{
    Transform t;  // i campi mancanti restano al valore di base
    if (!v.isObject())
        return t;
    const auto number = [&](const char* key, double& target) {
        if (v[key].isNumber() && std::isfinite(v[key].asNumber()))
            target = v[key].asNumber();
    };
    const auto flag = [&](const char* key, bool& target) {
        if (v[key].isBool())
            target = v[key].asBool();
    };
    number("scalaX", t.scaleX);
    number("scalaY", t.scaleY);
    number("posizioneX", t.posX);
    number("posizioneY", t.posY);
    number("rotazione", t.rotation);
    number("ritaglioSinistra", t.cropLeft);
    number("ritaglioDestra", t.cropRight);
    number("ritaglioAlto", t.cropTop);
    number("ritaglioBasso", t.cropBottom);
    flag("specchioOrizzontale", t.flipHorizontal);
    flag("specchioVerticale", t.flipVertical);
    number("opacita", t.opacity);
    return normalized(t);
}

JsonValue envelopeToJson(const std::vector<LevelPoint>& points)
{
    JsonValue::Array line;
    for (const LevelPoint& p : points)
        line.emplace_back(JsonValue::Array{ticks(p.time), p.db});
    return line;
}

std::vector<LevelPoint> envelopeFromJson(const JsonValue& value)
{
    std::vector<LevelPoint> points;
    if (!value.isArray())
        return points;
    for (const JsonValue& p : value.asArray()) {
        if (p.isArray() && p.asArray().size() == 2 && p.asArray()[0].isNumber() && p.asArray()[1].isNumber())
            points.push_back({ticksToSeconds(static_cast<std::int64_t>(p.asArray()[0].asNumber())), p.asArray()[1].asNumber()});
    }
    return normalizeEnvelope(std::move(points));
}

// Gli effetti di una traccia; un tipo che non si conosce si scarta (progetto di una versione più nuova).
std::vector<AudioEffect> effectsFromJson(const JsonValue& value)
{
    std::vector<AudioEffect> effects;
    if (!value.isArray())
        return effects;
    for (const JsonValue& item : value.asArray()) {
        if (item.isObject() && text(item["tipo"]) == "normalizza") {
            AudioEffect n;
            n.kind = EffectKind::Normalize;
            n.enabled = !item["attivo"].isBool() || item["attivo"].asBool();
            if (item["livelloDb"].isNumber())
                n.targetDb = item["livelloDb"].asNumber();
            effects.push_back(clampedEffect(n));
            continue;
        }
        if (!item.isObject() || text(item["tipo"]) != "equalizzatore")
            continue;
        AudioEffect e;
        e.kind = EffectKind::Equalizer;
        e.enabled = !item["attivo"].isBool() || item["attivo"].asBool();
        if (item["bande"].isArray()) {
            e.bands.clear();
            for (const JsonValue& b : item["bande"].asArray()) {
                EqBand band;
                const std::string type = text(b["tipo"]);
                band.type = type == "tagliaBassi" ? EqBandType::LowCut
                          : type == "shelfBassi"  ? EqBandType::LowShelf
                          : type == "shelfAlti"   ? EqBandType::HighShelf
                          : type == "tagliaAlti"  ? EqBandType::HighCut
                                                  : EqBandType::Bell;
                band.enabled = !b["attiva"].isBool() || b["attiva"].asBool();
                if (b["frequenza"].isNumber())
                    band.freq = b["frequenza"].asNumber();
                if (b["guadagno"].isNumber())
                    band.gainDb = b["guadagno"].asNumber();
                if (b["q"].isNumber())
                    band.q = b["q"].asNumber();
                e.bands.push_back(band);
            }
        } else if (item["parametri"].isArray()) {
            // Il primo equalizzatore (3 guadagni: bassi, medi, alti): diventa shelf a 120 Hz, campana a 1 kHz e shelf a 6 kHz.
            double gains[3] = {0, 0, 0};
            std::size_t i = 0;
            for (const JsonValue& v : item["parametri"].asArray()) {
                if (i < 3 && v.isNumber())
                    gains[i] = v.asNumber();
                ++i;
            }
            e.bands = {{EqBandType::LowShelf, true, 120.0, gains[0], 0.71},
                       {EqBandType::Bell, true, 1000.0, gains[1], 0.9},
                       {EqBandType::HighShelf, true, 6000.0, gains[2], 0.71}};
        }
        effects.push_back(clampedEffect(e));
    }
    return effects;
}

AudioMix mixFromJson(const JsonValue& value)
{
    AudioMix mix;
    if (!value.isObject())
        return mix;
    if (value["livello"].isNumber())
        mix.levelDb = std::clamp(value["livello"].asNumber(), AudioTracks::kMinLevelDb, AudioTracks::kMaxLevelDb);
    mix.muted = value["muto"].isBool() && value["muto"].asBool();
    mix.solo = value["solo"].isBool() && value["solo"].asBool();
    if (value["pan"].isNumber())
        mix.pan = std::clamp(value["pan"].asNumber(), -1.0, 1.0);
    mix.effects = effectsFromJson(value["effetti"]);
    return mix;
}

// Le tracce audio, quando non sono né quelle di partenza né una traccia sostitutiva (che resta nel formato di prima).
// Velocità e inverso di un pezzo o di una clip audio: scritti solo se diversi dal normale (1x, in avanti).
template <typename T>
void speedToJson(JsonValue::Object& object, const T& item)
{
    if (std::abs(item.speed - 1.0) > 1e-9)
        object["velocita"] = item.speed;
    if (item.reverse)
        object["inverso"] = true;
}

template <typename T>
void speedFromJson(const JsonValue& json, T& item)
{
    item.speed = json["velocita"].isNumber() ? clampSpeed(json["velocita"].asNumber()) : 1.0;
    item.reverse = json["inverso"].isBool() && json["inverso"].asBool();
}

JsonValue audioTracksToJson(const std::filesystem::path& dir, const AudioTracks& audio)
{
    JsonValue::Array tracks;
    for (const AudioTrack& track : audio.tracks()) {
        JsonValue::Array clips;
        for (const AudioClip& c : track.clips) {
            JsonValue::Object clip{{"percorso", c.path},
                                   {"relativo", relativeTo(dir, c.path)},
                                   {"inizio", ticks(c.start)},
                                   {"fine", ticks(c.end)},
                                   {"posizione", ticks(c.position)}};
            speedToJson(clip, c);
            if (c.sourceDuration > 0.0)
                clip["durataFile"] = ticks(c.sourceDuration);
            if (!c.envelope.empty())
                clip["linea"] = envelopeToJson(c.envelope);
            clips.emplace_back(std::move(clip));
        }
        JsonValue::Object item = mixToJson(track.mix).asObject();
        item["clip"] = std::move(clips);
        if (track.host)
            item["suA1"] = true;
        tracks.emplace_back(std::move(item));
    }
    JsonValue::Object root{{"A1", mixToJson(audio.mix(AudioTracks::kOriginalTrack))}, {"tracce", std::move(tracks)}};
    if (audio.masterLevelDb() != 0.0 || !audio.effects(AudioTracks::kMasterTrack).empty()) {
        AudioMix master;  // solo se c'è qualcosa: livello diverso da 0 dB o effetti
        master.levelDb = audio.masterLevelDb();
        master.effects = audio.effects(AudioTracks::kMasterTrack);
        root["master"] = mixToJson(master);
    }
    return root;
}

// Le scritte: colori come "#RRGGBBAA", grandezze e posizioni in frazioni del fotogramma, tempi in tick.
std::string colorToText(const TextColor& c)
{
    static const char digits[] = "0123456789abcdef";
    std::string out = "#";
    for (const std::uint8_t v : {c.r, c.g, c.b, c.a}) {
        out += digits[v >> 4];
        out += digits[v & 15];
    }
    return out;
}

TextColor colorFromText(const JsonValue& value, TextColor fallback)
{
    if (!value.isString())
        return fallback;
    const std::string& t = value.asString();
    if (t.size() != 9 || t[0] != '#')
        return fallback;
    const auto nibble = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
    std::uint8_t v[4];
    for (int i = 0; i < 4; ++i) {
        const int hi = nibble(t[static_cast<std::size_t>(1 + 2 * i)]);
        const int lo = nibble(t[static_cast<std::size_t>(2 + 2 * i)]);
        if (hi < 0 || lo < 0)
            return fallback;
        v[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return {v[0], v[1], v[2], v[3]};
}

const char* alignName(TextAlign a)
{
    return a == TextAlign::Left ? "sinistra" : a == TextAlign::Right ? "destra" : "centro";
}

TextAlign alignFromName(const JsonValue& value)
{
    if (value.isString() && value.asString() == "sinistra")
        return TextAlign::Left;
    if (value.isString() && value.asString() == "destra")
        return TextAlign::Right;
    return TextAlign::Center;
}

JsonValue textStyleToJson(const TextStyle& s)
{
    return JsonValue::Object{{"font", s.fontFamily},
                             {"grandezza", s.size},
                             {"grassetto", s.bold},
                             {"corsivo", s.italic},
                             {"sottolineato", s.underline},
                             {"allineamento", alignName(s.align)},
                             {"interlinea", s.lineSpacing},
                             {"colore", colorToText(s.color)},
                             {"opacita", s.opacity},
                             {"contorno", s.outline},
                             {"coloreContorno", colorToText(s.outlineColor)},
                             {"spessoreContorno", s.outlineWidth},
                             {"ombra", s.shadow},
                             {"coloreOmbra", colorToText(s.shadowColor)},
                             {"ombraX", s.shadowOffsetX},
                             {"ombraY", s.shadowOffsetY},
                             {"sfondo", s.background},
                             {"coloreSfondo", colorToText(s.backgroundColor)},
                             {"margineSfondo", s.backgroundPadding},
                             {"centroX", s.centerX},
                             {"centroY", s.centerY}};
}

TextStyle textStyleFromJson(const JsonValue& v)
{
    TextStyle s;  // i campi mancanti restano al valore di partenza
    if (!v.isObject())
        return s;
    const auto number = [&](const char* key, double fallback, double lo, double hi) {
        return v[key].isNumber() && std::isfinite(v[key].asNumber()) ? std::clamp(v[key].asNumber(), lo, hi) : fallback;
    };
    const auto flag = [&](const char* key, bool fallback) { return v[key].isBool() ? v[key].asBool() : fallback; };
    if (v["font"].isString() && !v["font"].asString().empty())
        s.fontFamily = v["font"].asString();
    s.size = number("grandezza", s.size, 0.005, 1.0);
    s.bold = flag("grassetto", s.bold);
    s.italic = flag("corsivo", s.italic);
    s.underline = flag("sottolineato", s.underline);
    s.align = alignFromName(v["allineamento"]);
    s.lineSpacing = number("interlinea", s.lineSpacing, 0.5, 3.0);
    s.color = colorFromText(v["colore"], s.color);
    s.opacity = number("opacita", s.opacity, 0.0, 1.0);
    s.outline = flag("contorno", s.outline);
    s.outlineColor = colorFromText(v["coloreContorno"], s.outlineColor);
    s.outlineWidth = number("spessoreContorno", s.outlineWidth, 0.0, 0.1);
    s.shadow = flag("ombra", s.shadow);
    s.shadowColor = colorFromText(v["coloreOmbra"], s.shadowColor);
    s.shadowOffsetX = number("ombraX", s.shadowOffsetX, -0.1, 0.1);
    s.shadowOffsetY = number("ombraY", s.shadowOffsetY, -0.1, 0.1);
    s.background = flag("sfondo", s.background);
    s.backgroundColor = colorFromText(v["coloreSfondo"], s.backgroundColor);
    s.backgroundPadding = number("margineSfondo", s.backgroundPadding, 0.0, 0.2);
    s.centerX = number("centroX", s.centerX, 0.0, 1.0);
    s.centerY = number("centroY", s.centerY, 0.0, 1.0);
    return s;
}

const char* directionName(TransitionDirection d)
{
    return d == TransitionDirection::Right ? "destra" : d == TransitionDirection::Up ? "alto" : d == TransitionDirection::Down ? "basso" : "sinistra";
}

TransitionDirection directionFromName(const JsonValue& value)
{
    const std::string name = text(value);
    return name == "destra" ? TransitionDirection::Right
           : name == "alto" ? TransitionDirection::Up
           : name == "basso" ? TransitionDirection::Down
                             : TransitionDirection::Left;
}

// Le transizioni: ognuna per posizione del pezzo prima della giunzione (gli id cambiano riaprendo il progetto).
JsonValue transitionsToJson(const Project& project)
{
    JsonValue::Array list;
    const auto& fragments = project.sequence().fragments();
    for (const TransitionClip& clip : project.transitions().clips()) {
        const int index = project.sequence().indexOf(clip.beforeFragment);
        if (index < 0 || static_cast<std::size_t>(index) + 1 >= fragments.size()
            || fragments[static_cast<std::size_t>(index) + 1].id != clip.afterFragment)
            continue;  // la giunzione non c'è più
        list.emplace_back(JsonValue::Object{{"dopoPezzo", index},
                                            {"tipo", clip.type},
                                            {"durata", ticks(clip.duration)},
                                            {"direzione", directionName(clip.params.direction)},
                                            {"audio", clip.crossfadeAudio}});
    }
    return list;
}

JsonValue textClipsToJson(const std::vector<TextClip>& list)
{
    JsonValue::Array clips;
    for (const TextClip& c : list) {
        JsonValue::Object clip{{"posizione", ticks(c.position)},
                               {"durata", ticks(c.length)},
                               {"testo", c.text},
                               {"stile", textStyleToJson(c.style)}};
        if (!c.transform.isIdentity())
            clip["trasformazione"] = transformToJson(c.transform);
        if (!c.animation.empty())
            clip["animazione"] = animationToJson(c.animation);  // tempi in secondi dall'inizio della scritta
        if (c.fadeIn > 0.0)
            clip["dissolvenzaIn"] = ticks(c.fadeIn);
        if (c.fadeOut > 0.0)
            clip["dissolvenzaOut"] = ticks(c.fadeOut);
        clips.emplace_back(std::move(clip));
    }
    return clips;
}

// Le scritte di un elenco ("clip" di un livello, "scrittePrincipale" di V1); vuoto se `value` non è un elenco.
std::vector<TextClip> textClipsFromJson(const JsonValue& value)
{
    std::vector<TextClip> list;
    if (!value.isArray())
        return list;
    for (const JsonValue& c : value.asArray()) {
        TextClip clip;
        clip.position = seconds(c["posizione"], "posizione");
        clip.length = seconds(c["durata"], "durata");
        clip.text = text(c["testo"]);
        clip.style = textStyleFromJson(c["stile"]);
        clip.transform = transformFromJson(c["trasformazione"]);
        clip.animation = animationFromJson(c["animazione"]);
        if (c["dissolvenzaIn"].isInteger())
            clip.fadeIn = std::clamp(ticksToSeconds(c["dissolvenzaIn"].asInteger()), 0.0, clip.length);
        if (c["dissolvenzaOut"].isInteger())
            clip.fadeOut = std::clamp(ticksToSeconds(c["dissolvenzaOut"].asInteger()), 0.0, clip.length);
        if (!(clip.length >= TextTracks::kMinLength) || clip.position < 0.0)
            throw ProjectFileError("Il progetto è rovinato: una scritta non è valida.");
        list.push_back(std::move(clip));
    }
    return list;
}

JsonValue textTracksToJson(const TextTracks& text)
{
    JsonValue::Array layers;
    for (const TextLayer& layer : text.layers()) {
        JsonValue clips = textClipsToJson(layer.clips);
        JsonValue::Object layerJson{{"clip", std::move(clips)}};
        if (!layer.pieces.empty()) {
            JsonValue::Array pieces;
            for (const LayerPiece& p : layer.pieces) {
                JsonValue::Object piece{{"file", p.clipId},
                                        {"inizio", ticks(p.start)},
                                        {"fine", ticks(p.end)},
                                        {"posizione", ticks(p.position)}};
                speedToJson(piece, p);
                if (!p.transform.isIdentity())
                    piece["trasformazione"] = transformToJson(p.transform);
                if (!p.animation.empty())
                    piece["animazione"] = animationToJson(p.animation);
                pieces.emplace_back(std::move(piece));
            }
            layerJson["pezzi"] = std::move(pieces);
        }
        layers.emplace_back(std::move(layerJson));
    }
    return layers;
}

JsonValue textStylesToJson(const std::vector<NamedTextStyle>& styles)
{
    JsonValue::Array list;
    for (const NamedTextStyle& named : styles)
        list.emplace_back(JsonValue::Object{{"nome", named.name}, {"stile", textStyleToJson(named.style)}});
    return list;
}

std::vector<NamedTextStyle> textStylesFromJsonValue(const JsonValue& value)
{
    std::vector<NamedTextStyle> styles;
    if (!value.isArray())
        return styles;
    for (const JsonValue& item : value.asArray()) {
        NamedTextStyle named;
        named.name = text(item["nome"]);
        if (named.name.empty())
            continue;
        named.style = textStyleFromJson(item["stile"]);
        const bool duplicate = std::any_of(styles.begin(), styles.end(), [&](const NamedTextStyle& s) { return s.name == named.name; });
        if (!duplicate)
            styles.push_back(std::move(named));
    }
    return styles;
}

// Un file audio delle tracce che non si trova (una volta sola, anche se lo usano più clip).
void addMissingAudio(LoadedProject& loaded, const std::string& absolute, const std::string& relative)
{
    const bool known = std::any_of(loaded.missing.begin(), loaded.missing.end(), [&](const MissingFile& m) {
        return m.clipId == 0 && m.savedPath == absolute;
    });
    if (!known)
        loaded.missing.push_back({0, absolute, fileNameOf(absolute.empty() ? relative : absolute)});
}

}  // namespace

std::int64_t secondsToTicks(double seconds)
{
    return std::llround(seconds * static_cast<double>(kProjectTicksPerSecond));
}

double ticksToSeconds(std::int64_t ticks)
{
    return static_cast<double>(ticks) / static_cast<double>(kProjectTicksPerSecond);
}

std::string projectToJson(const std::string& projectPath, const Project& project, const ProjectView& view)
{
    const std::filesystem::path dir = projectDir(projectPath);

    JsonValue::Array files;
    for (const Clip& clip : project.clips()) {
        JsonValue::Object file{{"id", clip.id}, {"percorso", clip.path}, {"relativo", relativeTo(dir, clip.path)}};
        file["durata"] = clip.analyzed ? ticks(clip.duration) : JsonValue();
        if (const auto proxy = view.proxyDivisors.find(clip.id); proxy != view.proxyDivisors.end() && proxy->second > 1)
            file["proxy"] = proxy->second;
        files.emplace_back(std::move(file));
    }

    JsonValue::Array sequence;
    for (const Fragment& f : project.sequence().fragments()) {
        JsonValue::Array muted;
        for (const TimeRange& r : f.muted)
            muted.emplace_back(JsonValue::Array{ticks(r.start), ticks(r.end)});
        JsonValue::Object piece{
            {"file", f.clipId}, {"inizio", ticks(f.start)}, {"fine", ticks(f.end)}, {"senzaAudio", std::move(muted)}};
        speedToJson(piece, f);
        if (f.gap > 0.0)
            piece["spazioPrima"] = ticks(f.gap);
        if (!f.envelope.empty())
            piece["linea"] = envelopeToJson(f.envelope);
        if (f.detached)
            piece["audioScollegato"] = true;
        if (!f.transform.isIdentity())
            piece["trasformazione"] = transformToJson(f.transform);
        if (!f.animation.empty())
            piece["animazione"] = animationToJson(f.animation);
        sequence.emplace_back(std::move(piece));
    }

    JsonValue replacement;
    JsonValue audioTracks;
    if (const auto r = project.audioReplacement()) {
        replacement = JsonValue::Object{{"percorso", r->path},
                                        {"relativo", relativeTo(dir, r->path)},
                                        {"scostamento", ticks(r->offset)},
                                        {"durata", ticks(r->duration)}};
    } else if (!project.audioTracks().isDefault()) {
        audioTracks = audioTracksToJson(dir, project.audioTracks());
    }

    // I gruppi: ogni oggetto per posizione (pezzo nella sequenza, clip audio per traccia e posizione nella traccia), gli id
    // cambiano riaprendo il progetto.
    JsonValue::Array groups;
    JsonValue::Array groupNames;  // in parallelo a `groups`, scritto solo se almeno un gruppo ha un nome
    bool anyGroupName = false;
    for (const ObjectGroup& group : project.validGroupList()) {
        JsonValue::Array items;
        groupNames.emplace_back(group.name);
        anyGroupName = anyGroupName || !group.name.empty();
        for (const ObjectRef& o : group.members) {
            if (o.kind == ObjectRef::Kind::TextClip) {
                int layerIndex = -1;
                int clipIndex = -1;
                const auto& baseClips = project.textTracks().baseLayer().clips;  // V1: traccia -2
                for (std::size_t c = 0; c < baseClips.size(); ++c) {
                    if (baseClips[c].id == o.id) {
                        layerIndex = -2;
                        clipIndex = static_cast<int>(c);
                    }
                }
                for (std::size_t l = 0; l < project.textTracks().layers().size(); ++l) {
                    const auto& clips = project.textTracks().layers()[l].clips;
                    for (std::size_t c = 0; c < clips.size(); ++c) {
                        if (clips[c].id == o.id) {
                            layerIndex = static_cast<int>(l);
                            clipIndex = static_cast<int>(c);
                        }
                    }
                }
                items.emplace_back(JsonValue::Object{{"tipo", "scritta"}, {"traccia", layerIndex}, {"posizione", clipIndex}});
            } else if (o.kind == ObjectRef::Kind::LayerPiece) {
                int layerIndex = -1;
                int pieceIndex = -1;
                for (std::size_t l = 0; l < project.textTracks().layers().size(); ++l) {
                    const auto& pieces = project.textTracks().layers()[l].pieces;
                    for (std::size_t c = 0; c < pieces.size(); ++c) {
                        if (pieces[c].id == o.id) {
                            layerIndex = static_cast<int>(l);
                            pieceIndex = static_cast<int>(c);
                        }
                    }
                }
                items.emplace_back(JsonValue::Object{{"tipo", "pezzoLivello"}, {"traccia", layerIndex}, {"posizione", pieceIndex}});
            } else if (o.kind == ObjectRef::Kind::AudioClip) {
                int trackIndex = -1;
                int clipIndex = -1;
                for (std::size_t t = 0; t < project.audioTracks().tracks().size(); ++t) {
                    const auto& clips = project.audioTracks().tracks()[t].clips;
                    for (std::size_t c = 0; c < clips.size(); ++c) {
                        if (clips[c].id == o.id) {
                            trackIndex = static_cast<int>(t);
                            clipIndex = static_cast<int>(c);
                        }
                    }
                }
                items.emplace_back(JsonValue::Object{{"tipo", "clip"}, {"traccia", trackIndex}, {"posizione", clipIndex}});
            } else {
                items.emplace_back(JsonValue::Object{{"tipo", o.kind == ObjectRef::Kind::Piece ? "pezzo" : "audioPezzo"},
                                                     {"posizione", project.sequence().indexOf(o.id)}});
            }
        }
        groups.emplace_back(std::move(items));
    }
    const JsonValue transitions = transitionsToJson(project);
    JsonValue::Object root{
        {"formato", kFormatName},
        {"versione", kProjectFormatVersion},  // sempre la versione attuale; si leggono anche tutte le precedenti
        {"formatoProgetto", formatToJson(project.format())},
        {"unitaDiTempo", JsonValue::Object{{"tickAlSecondo", kProjectTicksPerSecond},
                                           {"nota", "i tempi sono numeri interi di tick (1/705600000 di secondo)"}}},
        {"file", std::move(files)},
        {"sequenza", std::move(sequence)},
        {"audioSostitutivo", std::move(replacement)},
        {"sorgente", JsonValue::Object{{"file", view.sourceClipId},
                                       {"in", optionalTicks(view.in)},
                                       {"out", optionalTicks(view.out)},
                                       {"posizione", ticks(view.sourcePosition)}}},
        {"montaggio", JsonValue::Object{{"posizione", ticks(view.programPosition)}}},
    };
    if (!audioTracks.isNull())
        root["tracceAudio"] = std::move(audioTracks);
    if (!groups.empty())
        root["gruppi"] = std::move(groups);
    if (anyGroupName)
        root["nomiGruppi"] = std::move(groupNames);
    if (!project.textTracks().layers().empty())
        root["scritte"] = textTracksToJson(project.textTracks());
    if (!project.textTracks().baseLayer().clips.empty())  // le scritte su V1, anche senza livelli
        root["scrittePrincipale"] = textClipsToJson(project.textTracks().baseLayer().clips);
    if (!project.textStyles().empty())
        root["stiliScritte"] = textStylesToJson(project.textStyles());
    if (!transitions.asArray().empty())
        root["transizioni"] = transitions;
    return JsonValue(root).dump();
}

void saveProjectFile(const std::string& projectPath, const Project& project, const ProjectView& view)
{
    const std::string json = projectToJson(projectPath, project, view);
    const std::filesystem::path target = pathFromUtf8(projectPath);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.close();
        if (!out)
            throw ProjectFileError("Impossibile scrivere il progetto: " + projectPath);
    }
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        throw ProjectFileError("Impossibile salvare il progetto in " + projectPath + " (" + ec.message() + ").");
    }
}

LoadedProject loadProjectFile(const std::string& projectPath)
{
    std::ifstream in(pathFromUtf8(projectPath), std::ios::binary);
    if (!in)
        throw ProjectFileError("Impossibile aprire il progetto: " + projectPath);
    std::ostringstream content;
    content << in.rdbuf();
    return projectFromJson(projectPath, content.str());
}

LoadedProject projectFromJson(const std::string& projectPath, const std::string& json)
{
    const std::string name = fileNameOf(projectPath);
    JsonValue root;
    try {
        root = parseJson(json);
    } catch (const JsonError& e) {
        throw ProjectFileError(name + " non è un progetto di EditorVideo valido: il file è rovinato (" + e.what() + ").");
    }
    if (!root.isObject() || text(root["formato"]) != kFormatName)
        throw ProjectFileError(name + " non è un progetto di EditorVideo.");
    if (!root["versione"].isInteger())
        throw ProjectFileError(name + " non è un progetto di EditorVideo valido: manca la versione del formato.");
    const std::int64_t version = root["versione"].asInteger();
    if (version > kProjectFormatVersion || version < 1)
        throw ProjectFileError(name + " è stato salvato con una versione più recente di EditorVideo (formato "
                               + std::to_string(version) + ", questa versione legge fino al "
                               + std::to_string(kProjectFormatVersion) + "): aggiorna il programma per aprirlo.");
    if (root["unitaDiTempo"]["tickAlSecondo"].isInteger()
        && root["unitaDiTempo"]["tickAlSecondo"].asInteger() != kProjectTicksPerSecond)
        throw ProjectFileError(name + " usa un'unità di tempo sconosciuta.");

    const std::filesystem::path dir = projectDir(projectPath);
    LoadedProject loaded;
    try {
        // File: gli id salvati diventano quelli nuovi del progetto, nello stesso ordine.
        std::map<std::int64_t, int> ids;
        for (const JsonValue& file : root["file"].asArray()) {
            const std::string absolute = text(file["percorso"]);
            const std::string relative = text(file["relativo"]);
            if (absolute.empty() && relative.empty())
                throw ProjectFileError(name + " è rovinato: un file non ha il percorso.");
            const auto found = locate(dir, absolute, relative);
            const int id = loaded.project.add(found ? *found : absolute);
            ids[file["id"].asInteger()] = id;
            if (!found)
                loaded.missing.push_back({id, absolute, fileNameOf(absolute.empty() ? relative : absolute)});
            if (file["durata"].isInteger())
                loaded.savedDurations[id] = ticksToSeconds(file["durata"].asInteger());
            if (file["proxy"].isInteger()) {
                const auto divisor = file["proxy"].asInteger();
                if (divisor == 2 || divisor == 4 || divisor == 8)  // valori diversi: si ignora, anteprima dall'originale
                    loaded.view.proxyDivisors[id] = static_cast<int>(divisor);
            }
        }

        std::vector<Fragment> fragments;
        for (const JsonValue& piece : root["sequenza"].asArray()) {
            const auto id = ids.find(piece["file"].asInteger());
            if (id == ids.end())
                throw ProjectFileError(name + " è rovinato: un pezzo della sequenza indica un file che non c'è.");
            Fragment f;
            f.clipId = id->second;
            f.start = seconds(piece["inizio"], "inizio");
            f.end = seconds(piece["fine"], "fine");
            if (!(f.end > f.start))
                throw ProjectFileError(name + " è rovinato: un pezzo della sequenza finisce prima di iniziare.");
            if (piece["senzaAudio"].isArray()) {
                for (const JsonValue& range : piece["senzaAudio"].asArray()) {
                    const auto& pair = range.asArray();
                    if (pair.size() != 2)
                        throw ProjectFileError(name + " è rovinato: un tratto senza audio non è valido.");
                    f.muted.push_back({seconds(pair[0], "senzaAudio"), seconds(pair[1], "senzaAudio")});
                }
            }
            speedFromJson(piece, f);
            f.envelope = envelopeFromJson(piece["linea"]);
            f.detached = piece["audioScollegato"].isBool() && piece["audioScollegato"].asBool();
            f.transform = transformFromJson(piece["trasformazione"]);
            f.animation = animationFromJson(piece["animazione"]);
            if (piece["spazioPrima"].isInteger()) {
                f.gap = seconds(piece["spazioPrima"], "spazioPrima");
                if (f.gap < 0.0)
                    throw ProjectFileError(name + " è rovinato: uno spazio vuoto della sequenza è negativo.");
            }
            fragments.push_back(std::move(f));
        }
        loaded.project.sequence().restore(std::move(fragments));

        if (const JsonValue& r = root["audioSostitutivo"]; r.isObject()) {
            const std::string absolute = text(r["percorso"]);
            const auto found = locate(dir, absolute, text(r["relativo"]));
            loaded.project.setAudioReplacement(
                AudioReplacement{found ? *found : absolute, seconds(r["scostamento"], "scostamento"),
                                 r["durata"].isInteger() ? ticksToSeconds(r["durata"].asInteger()) : 0.0});
            if (!found)
                addMissingAudio(loaded, absolute, text(r["relativo"]));
        }

        if (const JsonValue& a = root["tracceAudio"]; a.isObject()) {
            std::vector<AudioTrack> tracks;
            for (const JsonValue& t : a["tracce"].asArray()) {
                AudioTrack track;
                track.mix = mixFromJson(t);
                track.host = t["suA1"].isBool() && t["suA1"].asBool();
                for (const JsonValue& c : t["clip"].asArray()) {
                    const std::string absolute = text(c["percorso"]);
                    const std::string relative = text(c["relativo"]);
                    if (absolute.empty() && relative.empty())
                        throw ProjectFileError(name + " è rovinato: una clip audio non ha il percorso del file.");
                    const auto found = locate(dir, absolute, relative);
                    AudioClip clip;
                    clip.path = found ? *found : absolute;
                    clip.start = seconds(c["inizio"], "inizio");
                    clip.end = seconds(c["fine"], "fine");
                    clip.position = seconds(c["posizione"], "posizione");
                    if (c["durataFile"].isInteger())
                        clip.sourceDuration = ticksToSeconds(c["durataFile"].asInteger());
                    clip.envelope = envelopeFromJson(c["linea"]);
                    speedFromJson(c, clip);
                    if (!(clip.end > clip.start) || clip.start < 0.0 || clip.position < 0.0)
                        throw ProjectFileError(name + " è rovinato: una clip audio non è valida.");
                    if (!found)
                        addMissingAudio(loaded, absolute, relative);
                    track.clips.push_back(std::move(clip));
                }
                tracks.push_back(std::move(track));
            }
            loaded.project.audioTracks().restore(mixFromJson(a["A1"]), std::move(tracks), mixFromJson(a["master"]).levelDb,
                                                        mixFromJson(a["master"]).effects);
        }

        // Scritte: una scritta con durata troppo corta o prima di 0 rende rovinato il file; una sovrapposta a un'altra si scarta.
        if (const JsonValue& t = root["scritte"]; t.isArray()) {
            std::vector<TextLayer> layers;
            for (const JsonValue& l : t.asArray()) {
                TextLayer layer;
                layer.clips = textClipsFromJson(l["clip"]);
                // Pezzi di video del livello (versione 7): il file deve esserci, durata e posizione valide.
                if (l["pezzi"].isArray()) {
                    for (const JsonValue& p : l["pezzi"].asArray()) {
                        const auto file = ids.find(p["file"].asInteger());
                        if (file == ids.end())
                            throw ProjectFileError(name + " è rovinato: un pezzo di un livello indica un file che non c'è.");
                        LayerPiece piece;
                        piece.clipId = file->second;
                        piece.start = seconds(p["inizio"], "inizio");
                        piece.end = seconds(p["fine"], "fine");
                        piece.position = seconds(p["posizione"], "posizione");
                        piece.transform = transformFromJson(p["trasformazione"]);
                        piece.animation = animationFromJson(p["animazione"]);
                        speedFromJson(p, piece);
                        if (!(piece.length() >= TextTracks::kMinLength) || piece.position < 0.0 || piece.start < 0.0)
                            throw ProjectFileError(name + " è rovinato: un pezzo di un livello non è valido.");
                        layer.pieces.push_back(std::move(piece));
                    }
                }
                layers.push_back(std::move(layer));
            }
            loaded.project.textTracks().restore(std::move(layers), textClipsFromJson(root["scrittePrincipale"]));
        }

        loaded.project.textStyles() = textStylesFromJsonValue(root["stiliScritte"]);

        // Gruppi: ogni oggetto è dato per posizione (vedi projectToJson); quelli che non ci sono più si scartano.
        if (root["gruppi"].isArray()) {
            const Project& built = loaded.project;
            std::vector<ObjectGroup> groups;
            const JsonValue& names = root["nomiGruppi"];
            for (const JsonValue& group : root["gruppi"].asArray()) {
                std::vector<ObjectRef> members;
                for (const JsonValue& item : group.asArray()) {
                    const std::string kind = text(item["tipo"]);
                    const std::int64_t at = item["posizione"].isInteger() ? item["posizione"].asInteger() : -1;
                    if (kind == "scritta") {
                        const std::int64_t l = item["traccia"].isInteger() ? item["traccia"].asInteger() : -1;
                        const auto& layers = built.textTracks().layers();
                        if (l == -2) {
                            const auto& baseClips = built.textTracks().baseLayer().clips;
                            if (at >= 0 && at < static_cast<std::int64_t>(baseClips.size()))
                                members.push_back({ObjectRef::Kind::TextClip, baseClips[static_cast<std::size_t>(at)].id});
                        } else if (l >= 0 && l < static_cast<std::int64_t>(layers.size()) && at >= 0
                            && at < static_cast<std::int64_t>(layers[static_cast<std::size_t>(l)].clips.size()))
                            members.push_back({ObjectRef::Kind::TextClip, layers[static_cast<std::size_t>(l)].clips[static_cast<std::size_t>(at)].id});
                    } else if (kind == "pezzoLivello") {
                        const std::int64_t l = item["traccia"].isInteger() ? item["traccia"].asInteger() : -1;
                        const auto& layers = built.textTracks().layers();
                        if (l >= 0 && l < static_cast<std::int64_t>(layers.size()) && at >= 0
                            && at < static_cast<std::int64_t>(layers[static_cast<std::size_t>(l)].pieces.size()))
                            members.push_back({ObjectRef::Kind::LayerPiece, layers[static_cast<std::size_t>(l)].pieces[static_cast<std::size_t>(at)].id});
                    } else if (kind == "clip") {
                        const std::int64_t t = item["traccia"].isInteger() ? item["traccia"].asInteger() : -1;
                        const auto& tracks = built.audioTracks().tracks();
                        if (t >= 0 && t < static_cast<std::int64_t>(tracks.size()) && at >= 0
                            && at < static_cast<std::int64_t>(tracks[static_cast<std::size_t>(t)].clips.size()))
                            members.push_back({ObjectRef::Kind::AudioClip, tracks[static_cast<std::size_t>(t)].clips[static_cast<std::size_t>(at)].id});
                    } else if ((kind == "pezzo" || kind == "audioPezzo") && at >= 0
                               && at < static_cast<std::int64_t>(built.sequence().fragments().size())) {
                        members.push_back({kind == "pezzo" ? ObjectRef::Kind::Piece : ObjectRef::Kind::PieceAudio,
                                           built.sequence().fragments()[static_cast<std::size_t>(at)].id});
                    }
                }
                ObjectGroup loadedGroup;
                loadedGroup.members = std::move(members);
                if (names.isArray() && groups.size() < names.asArray().size() && names.asArray()[groups.size()].isString())
                    loadedGroup.name = text(names.asArray()[groups.size()]);
                groups.push_back(std::move(loadedGroup));
            }
            loaded.project.groups().restore(groups);
        }

        // Transizioni: ognuna è data dalla posizione del pezzo prima della giunzione; quelle senza giunzione o con una durata
        // che non ha senso si scartano (il controllo dei margini si fa quando i file sono analizzati).
        if (root["transizioni"].isArray()) {
            const auto& fragmentsBuilt = loaded.project.sequence().fragments();
            std::vector<TransitionClip> clips;
            for (const JsonValue& item : root["transizioni"].asArray()) {
                const std::int64_t at = item["dopoPezzo"].isInteger() ? item["dopoPezzo"].asInteger() : -1;
                if (at < 0 || at + 1 >= static_cast<std::int64_t>(fragmentsBuilt.size()) || !item["durata"].isInteger())
                    continue;
                TransitionClip clip;
                clip.type = text(item["tipo"]);
                clip.beforeFragment = fragmentsBuilt[static_cast<std::size_t>(at)].id;
                clip.afterFragment = fragmentsBuilt[static_cast<std::size_t>(at) + 1].id;
                clip.duration = ticksToSeconds(item["durata"].asInteger());
                clip.params.direction = directionFromName(item["direzione"]);
                clip.crossfadeAudio = !item["audio"].isBool() || item["audio"].asBool();
                if (clip.type.empty() || !(clip.duration > 0.0))
                    continue;
                clips.push_back(std::move(clip));
            }
            loaded.project.mutableTransitions().restore(std::move(clips));
        }

        // Formato del progetto. I progetti di prima non ce l'hanno: lo si ricava poi dal primo pezzo.
        if (const JsonValue& f = root["formatoProgetto"]; f.isObject() && f["flussi"].isArray()) {
            ProjectFormat format;
            format.sourceName = text(f["daFile"]);
            for (const JsonValue& s : f["flussi"].asArray()) {
                StreamSignature stream;
                stream.kind = text(s["tipo"]) == "video" ? StreamKind::Video : StreamKind::Audio;
                stream.codec = text(s["codec"]);
                stream.profile = text(s["profilo"]);
                stream.extradata = bytesOf(text(s["parametri"]));
                const auto number = [&](const char* key) { return s[key].isNumber() ? s[key].asNumber() : 0.0; };
                if (stream.kind == StreamKind::Video) {
                    stream.width = static_cast<int>(number("larghezza"));
                    stream.height = static_cast<int>(number("altezza"));
                    stream.pixelFormat = text(s["formatoPixel"]);
                    stream.frameRate = number("fotogrammiAlSecondo");
                } else {
                    stream.sampleRate = static_cast<int>(number("frequenza"));
                    stream.channels = static_cast<int>(number("canali"));
                    stream.channelLayout = text(s["disposizioneCanali"]);
                }
                if (stream.codec.empty())
                    throw ProjectFileError(name + " è rovinato: un flusso del formato del progetto non ha il codec.");
                format.streams.push_back(std::move(stream));
            }
            loaded.project.setFormat(std::move(format));
        }

        const JsonValue& source = root["sorgente"];
        if (source.isObject()) {
            const auto id = source["file"].isInteger() ? ids.find(source["file"].asInteger()) : ids.end();
            loaded.view.sourceClipId = id != ids.end() ? id->second : 0;
            if (source["in"].isInteger())
                loaded.view.in = ticksToSeconds(source["in"].asInteger());
            if (source["out"].isInteger())
                loaded.view.out = ticksToSeconds(source["out"].asInteger());
            if (source["posizione"].isInteger())
                loaded.view.sourcePosition = ticksToSeconds(source["posizione"].asInteger());
        }
        if (root["montaggio"]["posizione"].isInteger())
            loaded.view.programPosition = ticksToSeconds(root["montaggio"]["posizione"].asInteger());
    } catch (const JsonError& e) {
        throw ProjectFileError(name + " non è un progetto di EditorVideo valido (" + e.what() + ").");
    }
    return loaded;
}

void relocateMissing(LoadedProject& loaded, const MissingFile& file, const std::string& newPath)
{
    if (file.clipId == 0)
        loaded.project.audioTracks().relocate(file.savedPath, newPath);
    else
        loaded.project.setClipPath(file.clipId, newPath);
    std::erase_if(loaded.missing, [&](const MissingFile& m) {
        return m.clipId == file.clipId && (file.clipId != 0 || m.savedPath == file.savedPath);
    });
}

int dropMissing(LoadedProject& loaded)
{
    int removed = 0;
    for (const MissingFile& file : loaded.missing) {
        if (file.clipId == 0) {
            if (const auto replacement = loaded.project.audioReplacement(); replacement && replacement->path == file.savedPath)
                loaded.project.setAudioReplacement(std::nullopt);  // torna l'audio originale
            else
                loaded.project.audioTracks().removePath(file.savedPath);
            continue;
        }
        const auto& fragments = loaded.project.sequence().fragments();
        removed += static_cast<int>(std::count_if(fragments.begin(), fragments.end(),
                                                  [&](const Fragment& f) { return f.clipId == file.clipId; }));
        loaded.project.remove(file.clipId);
        loaded.savedDurations.erase(file.clipId);
        if (loaded.view.sourceClipId == file.clipId)
            loaded.view.sourceClipId = 0;
    }
    loaded.missing.clear();
    return removed;
}

std::string textStylesToText(const std::vector<NamedTextStyle>& styles)
{
    return JsonValue(textStylesToJson(styles)).dump();
}

std::vector<NamedTextStyle> textStylesFromText(const std::string& text)
{
    try {
        return textStylesFromJsonValue(parseJson(text));
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace editor::core
