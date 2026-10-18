#include "EditorCore/LibraryInfo.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace editor::core {

FfmpegInfo ffmpegInfo()
{
    return {
        av_version_info(),
        formatLibVersion(avformat_version()),
        formatLibVersion(avcodec_version()),
    };
}

std::string formatLibVersion(unsigned version)
{
    return std::to_string(AV_VERSION_MAJOR(version)) + "." + std::to_string(AV_VERSION_MINOR(version)) + "."
         + std::to_string(AV_VERSION_MICRO(version));
}

}  // namespace editor::core
