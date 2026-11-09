#include "EditorCore/FrameCompositor.h"

#include "EditorCore/GpuCompositor.h"
#include "EditorCore/TextFrame.h"

namespace editor::core {

std::unique_ptr<IFrameCompositor> makeFrameCompositor(int width, int height, AVPixelFormat pixelFormat, bool allowGpu)
{
    if (allowGpu)
        if (std::unique_ptr<GpuCompositor> gpu = GpuCompositor::create(width, height, pixelFormat))
            return gpu;
    return std::make_unique<TextFrameBlender>(width, height, pixelFormat);
}

}  // namespace editor::core
