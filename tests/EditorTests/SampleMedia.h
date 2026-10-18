#pragma once

#include <filesystem>
#include <string>

namespace editor::tests {

// File di prova generati al volo con FFmpeg, così non serve committare video binari.
struct SampleSpec
{
    bool video = true;  // MPEG-4 Part 2, 64x48, 25 fps, senza B-frame
    bool audio = false; // PCM 16 bit, 48 kHz, stereo
    int frames = 30;    // durata = frames / 25 secondi
    int gopSize = 10;   // un fotogramma chiave ogni gopSize fotogrammi
};

// Il formato del contenitore si ricava dall'estensione (.mkv, .mp4, .wav, ...). Lancia std::runtime_error se fallisce.
void writeSampleFile(const std::filesystem::path& path, const SampleSpec& spec);

// false se la build di FFmpeg non ha gli encoder usati da writeSampleFile.
bool sampleEncodersAvailable();

std::string toUtf8(const std::filesystem::path& path);

}  // namespace editor::tests
