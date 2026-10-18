#pragma once

#include <string>

namespace editor::core {

// Versioni delle librerie FFmpeg con cui l'applicazione è stata compilata.
struct FfmpegInfo
{
    std::string version;   // es. "7.1"
    std::string avformat;  // es. "61.7.100"
    std::string avcodec;
};

FfmpegInfo ffmpegInfo();

// Converte una versione nel formato AV_VERSION_INT in "major.minor.micro".
std::string formatLibVersion(unsigned version);

}  // namespace editor::core
