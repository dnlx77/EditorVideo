#include "EditorCore/FrameCompositor.h"

#include "EditorCore/TextFrame.h"

namespace editor::core {

std::unique_ptr<IFrameCompositor> makeFrameCompositor(int width, int height, AVPixelFormat pixelFormat)
{
    return std::make_unique<TextFrameBlender>(width, height, pixelFormat);
}

}  // namespace editor::core
