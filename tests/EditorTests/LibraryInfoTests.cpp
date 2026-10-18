#include "EditorCore/LibraryInfo.h"

#include <gtest/gtest.h>

extern "C" {
#include <libavutil/version.h>
}

using editor::core::ffmpegInfo;
using editor::core::formatLibVersion;

TEST(LibraryInfo, FormatsVersionAsMajorMinorMicro)
{
    EXPECT_EQ(formatLibVersion(AV_VERSION_INT(61, 7, 100)), "61.7.100");
    EXPECT_EQ(formatLibVersion(AV_VERSION_INT(0, 0, 0)), "0.0.0");
}

TEST(LibraryInfo, ReportsLinkedFfmpeg)
{
    const auto info = ffmpegInfo();
    EXPECT_FALSE(info.version.empty());
    EXPECT_FALSE(info.avformat.empty());
    EXPECT_FALSE(info.avcodec.empty());
}
