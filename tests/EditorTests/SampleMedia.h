#pragma once

#include "EditorCore/PathUtil.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace editor::tests {

// File di prova generati al volo con FFmpeg, così non serve committare video binari.
struct SampleSpec
{
    bool video = true;  // 25 fps
    int bFrames = 0;    // B-frame consecutivi al massimo (0 = nessuno)
    bool audio = false; // PCM 16 bit, 48 kHz, stereo
    int frames = 30;    // durata = frames / 25 secondi
    int gopSize = 10;   // un fotogramma chiave ogni gopSize fotogrammi
    bool mpeg2 = false; // MPEG-2 (decodificabile anche dalla GPU) invece di MPEG-4 Part 2 (solo software)
    int width = 64;
    int height = 48;
    bool silentAudio = false;  // audio a zero invece del segnale di prova (per provare l'anteprima senza rumore)
    bool aacAudio = false;     // audio AAC (come nei video dei telefoni) invece di PCM; serve per l'MP4
    bool mp2Audio = false;     // audio MPEG-1 Layer II invece di PCM (della stessa famiglia dell'MP3)
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

// I file "sorgente.mkv" (MPEG-4 + PCM) e "sorgente.mp4" (MPEG-4 + AAC) usati dalle prove di esportazione: 4 s,
// 100 fotogrammi, un fotogramma chiave ogni secondo.
void writeExportSources(const std::filesystem::path& dir);

// Base dei test che lavorano su file di prova generati in una cartella temporanea propria (una per gruppo di test).
// La classe derivata dichiara, pubblici, kDirName (nome della cartella in %TEMP%) e prepareSamples() (genera i file
// in dir_). Senza gli encoder di FFmpeg i file non si generano e i test vengono saltati.
template <typename Derived>
class SampleDirTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = std::filesystem::temp_directory_path() / Derived::kDirName;
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        if (sampleEncodersAvailable())
            Derived::prepareSamples();
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void SetUp() override
    {
        if (!sampleEncodersAvailable())
            GTEST_SKIP() << "FFmpeg senza gli encoder necessari: impossibile generare i file di prova";
    }

    // Percorso UTF-8 di un file della cartella.
    static std::string path(const std::filesystem::path& name) { return core::utf8FromPath(dir_ / name); }

    static inline std::filesystem::path dir_;
};

}  // namespace editor::tests
