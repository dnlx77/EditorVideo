#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace editor::tests {

// File di prova generati al volo con FFmpeg, così non serve committare video binari.
struct SampleSpec
{
    bool video = true;  // 25 fps, senza B-frame
    bool audio = false; // PCM 16 bit, 48 kHz, stereo
    int frames = 30;    // durata = frames / 25 secondi
    int gopSize = 10;   // un fotogramma chiave ogni gopSize fotogrammi
    bool mpeg2 = false; // MPEG-2 (decodificabile anche dalla GPU) invece di MPEG-4 Part 2 (solo software)
    int width = 64;
    int height = 48;
    bool silentAudio = false;  // audio a zero invece del segnale di prova (per provare l'anteprima senza rumore)
    bool aacAudio = false;     // audio AAC (come nei video dei telefoni) invece di PCM; serve per l'MP4
    int sampleRate = 48000;
    int firstIndex = 0;  // il contenuto (luminanza e audio) parte dal fotogramma firstIndex invece che da 0:
                         // serve a riconoscere i file uniti
};

// Valore della luminanza (Y) del pixel (x, y) nel fotogramma `index` dei file di prova.
int sampleLuma(int x, int y, int index);

// Valore del campione audio `n` (contato dall'inizio del file, per canale) nei file di prova.
std::int16_t sampleAudio(long long n);

// Il formato del contenitore si ricava dall'estensione (.mkv, .mp4, .wav, ...). Lancia std::runtime_error se fallisce.
void writeSampleFile(const std::filesystem::path& path, const SampleSpec& spec);

// false se la build di FFmpeg non ha gli encoder usati da writeSampleFile.
bool sampleEncodersAvailable();

std::string toUtf8(const std::filesystem::path& path);

}  // namespace editor::tests
